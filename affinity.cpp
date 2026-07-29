#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#error This benchmark is intentionally Windows-only. It uses VirtualAlloc, VirtualFree and SetThreadAffinityMask.
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <intrin.h>
#include <windows.h>
#include <psapi.h>

namespace {

constexpr std::size_t kBlockSize = 64;
constexpr std::uint64_t kPoolIterations = 10'000'000;
constexpr std::uint64_t kHeapIterations = 5'000'000;
constexpr std::uint64_t kVirtualAllocIterations = 50'000;
constexpr std::size_t kFastSamples = 50'000;
constexpr std::size_t kFastSampleBatch = 64;
constexpr std::size_t kVirtualSamples = 50'000;
constexpr std::size_t kPoolSlots = 4096;
constexpr std::size_t kBatchRegionBytes = 256ull * 1024ull * 1024ull;

#define NOINLINE __declspec(noinline)

std::atomic<std::uint64_t> g_sink{0};

struct alignas(64) Block64 {
    std::uint64_t fields[8];
};

static_assert(sizeof(Block64) == kBlockSize);

template <class T>
void do_not_optimize(const T& value) {
    const volatile auto* ptr = &value;
    (void)ptr;
    _ReadWriteBarrier();
}

void clobber_memory() {
    _ReadWriteBarrier();
}

NOINLINE Block64* allocate_heap_block() {
    return new Block64;
}

NOINLINE void free_heap_block(Block64* block) {
    delete block;
}

NOINLINE void* allocate_virtual_region(std::size_t bytes) {
    return VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT,
                        PAGE_READWRITE);
}

NOINLINE bool free_virtual_region(void* region) {
    return VirtualFree(region, 0, MEM_RELEASE) != FALSE;
}

std::uint64_t now_ns() {
    using clock = std::chrono::steady_clock;
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               clock::now().time_since_epoch())
        .count();
}

std::uint64_t measure_timer_overhead_ns() {
    constexpr std::size_t iterations = 100'000;
    std::uint64_t best = UINT64_MAX;

    for (std::size_t i = 0; i < iterations; ++i) {
        const std::uint64_t start = now_ns();
        const std::uint64_t end = now_ns();
        best = std::min(best, end - start);
    }

    return best == UINT64_MAX ? 0 : best;
}

struct PageFaultCounters {
    DWORD page_faults = 0;
};

PageFaultCounters read_page_faults() {
    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof(counters);

    if (!GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
        return {};
    }

    return {counters.PageFaultCount};
}

struct CpuSelection {
    std::array<DWORD, 4> cpus{0, 1, 2, 3};
};

struct Config {
    CpuSelection cpu_selection{};
    std::uint64_t pool_iterations = kPoolIterations;
    std::uint64_t heap_iterations = kHeapIterations;
    std::uint64_t virtual_iterations = kVirtualAllocIterations;
    std::size_t batch_region_bytes = kBatchRegionBytes;
};

struct AffinityResult {
    bool pinned = false;
    DWORD requested_cpu = 0;
    DWORD start_cpu = MAXDWORD;
    DWORD end_cpu = MAXDWORD;
    bool migrated = false;
};

AffinityResult pin_current_thread(DWORD cpu) {
    AffinityResult result{};
    result.requested_cpu = cpu;

    const DWORD_PTR mask = DWORD_PTR{1} << cpu;
    result.pinned = SetThreadAffinityMask(GetCurrentThread(), mask) != 0;
    result.start_cpu = GetCurrentProcessorNumber();
    return result;
}

void finish_affinity_result(AffinityResult& result) {
    result.end_cpu = GetCurrentProcessorNumber();
    result.migrated = result.start_cpu != MAXDWORD && result.end_cpu != MAXDWORD &&
                      result.start_cpu != result.end_cpu;
}

std::string affinity_text(const AffinityResult& affinity) {
    std::ostringstream out;
    out << "cpu=" << affinity.requested_cpu << " pinned="
        << (affinity.pinned ? "yes" : "no") << " start_cpu="
        << affinity.start_cpu << " end_cpu=" << affinity.end_cpu;
    if (affinity.migrated) {
        out << " WARNING:migrated";
    }
    return out.str();
}

std::size_t page_size() {
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    return info.dwPageSize;
}

class IntrusiveObjectPool {
public:
    explicit IntrusiveObjectPool(std::size_t slots) : slots_(slots) {
        const std::size_t bytes = slots_ * sizeof(Block64);
        memory_ = static_cast<Block64*>(allocate_virtual_region(bytes));
        if (memory_ == nullptr) {
            throw std::runtime_error("VirtualAlloc failed for object pool");
        }

        for (std::size_t i = 0; i < slots_; ++i) {
            auto* slot = reinterpret_cast<FreeNode*>(&memory_[i]);
            slot->next = free_list_;
            free_list_ = slot;
        }
    }

    IntrusiveObjectPool(const IntrusiveObjectPool&) = delete;
    IntrusiveObjectPool& operator=(const IntrusiveObjectPool&) = delete;

    ~IntrusiveObjectPool() {
        if (memory_ != nullptr) {
            free_virtual_region(memory_);
        }
    }

    void warm_up() {
        for (std::size_t i = 0; i < slots_; ++i) {
            memory_[i].fields[0] = static_cast<std::uint64_t>(i);
        }
        clobber_memory();
    }

    Block64* allocate() {
        FreeNode* node = free_list_;
        free_list_ = node->next;
        return reinterpret_cast<Block64*>(node);
    }

    void deallocate(Block64* block) {
        auto* node = reinterpret_cast<FreeNode*>(block);
        node->next = free_list_;
        free_list_ = node;
    }

private:
    struct FreeNode {
        FreeNode* next = nullptr;
    };

    std::size_t slots_ = 0;
    Block64* memory_ = nullptr;
    FreeNode* free_list_ = nullptr;
};

struct Stats {
    double mean_ns = 0.0;
    std::uint64_t min_ns = 0;
    std::uint64_t p50_ns = 0;
    std::uint64_t p90_ns = 0;
    std::uint64_t p99_ns = 0;
    std::uint64_t p999_ns = 0;
    std::optional<std::uint64_t> p9999_ns;
    std::uint64_t max_ns = 0;
};

std::uint64_t percentile(const std::vector<std::uint64_t>& samples,
                         double fraction) {
    if (samples.empty()) {
        return 0;
    }

    const double index = fraction * static_cast<double>(samples.size() - 1);
    return samples[static_cast<std::size_t>(index + 0.5)];
}

Stats build_stats(std::vector<std::uint64_t> samples, double mean_ns) {
    Stats stats{};
    stats.mean_ns = mean_ns;

    if (samples.empty()) {
        return stats;
    }

    std::sort(samples.begin(), samples.end());
    stats.min_ns = samples.front();
    stats.p50_ns = percentile(samples, 0.50);
    stats.p90_ns = percentile(samples, 0.90);
    stats.p99_ns = percentile(samples, 0.99);
    stats.p999_ns = percentile(samples, 0.999);
    if (samples.size() >= 10'000) {
        stats.p9999_ns = percentile(samples, 0.9999);
    }
    stats.max_ns = samples.back();

    return stats;
}

struct TestResult {
    std::string name;
    std::uint64_t iterations = 0;
    double elapsed_seconds = 0.0;
    Stats stats{};
    std::uint64_t checksum = 0;
    PageFaultCounters faults_before{};
    PageFaultCounters faults_after{};
    AffinityResult affinity{};
};

void touch_block(Block64* block, std::uint64_t value, std::uint64_t& checksum) {
    block->fields[0] = value;
    block->fields[1] = value + 1;
    block->fields[2] = value + 2;
    block->fields[3] = value + 3;
    checksum += block->fields[0] ^ block->fields[3];
    do_not_optimize(checksum);
}

void warm_up_heap() {
    std::vector<Block64*> blocks;
    blocks.reserve(kPoolSlots);

    for (std::size_t i = 0; i < kPoolSlots; ++i) {
        auto* block = allocate_heap_block();
        *block = {};
        block->fields[0] = i;
        blocks.push_back(block);
    }

    for (Block64* block : blocks) {
        free_heap_block(block);
    }
}

void warm_up_virtual_alloc(std::size_t bytes) {
    void* region = allocate_virtual_region(bytes);
    if (region == nullptr) {
        throw std::runtime_error("VirtualAlloc warm-up failed");
    }

    auto* data = static_cast<volatile unsigned char*>(region);
    data[0] = 1;
    do_not_optimize(data[0]);

    if (!free_virtual_region(region)) {
        throw std::runtime_error("VirtualFree warm-up failed");
    }
}

template <class Worker>
TestResult run_on_worker(const std::string& name, DWORD cpu, Worker worker) {
    TestResult result{};
    result.name = name;

    std::thread thread([&] {
        result.affinity = pin_current_thread(cpu);
        worker(result);
        finish_affinity_result(result.affinity);
    });

    thread.join();
    g_sink.fetch_xor(result.checksum, std::memory_order_relaxed);
    return result;
}

TestResult run_pool_test(DWORD cpu,
                         std::uint64_t iterations,
                         std::uint64_t timer_overhead_ns) {
    return run_on_worker("pool", cpu, [&](TestResult& result) {
        IntrusiveObjectPool pool(kPoolSlots);
        pool.warm_up();

        std::uint64_t checksum = 0;
        for (std::size_t i = 0; i < 1024; ++i) {
            Block64* block = pool.allocate();
            touch_block(block, i, checksum);
            pool.deallocate(block);
        }

        std::vector<std::uint64_t> samples;
        samples.reserve(kFastSamples);

        result.faults_before = read_page_faults();

        const auto start = std::chrono::steady_clock::now();
        for (std::uint64_t i = 0; i < iterations; ++i) {
            Block64* block = pool.allocate();
            touch_block(block, 0xA5A5A5A500000000ull + i, checksum);
            pool.deallocate(block);
        }
        const auto end = std::chrono::steady_clock::now();

        for (std::size_t sample = 0; sample < kFastSamples; ++sample) {
            const std::uint64_t sample_start = now_ns();
            for (std::size_t i = 0; i < kFastSampleBatch; ++i) {
                Block64* block = pool.allocate();
                touch_block(block, 0xC001000000000000ull + sample + i,
                            checksum);
                pool.deallocate(block);
            }
            const std::uint64_t sample_end = now_ns();
            const std::uint64_t corrected =
                sample_end - sample_start > timer_overhead_ns
                    ? sample_end - sample_start - timer_overhead_ns
                    : 0;
            samples.push_back(corrected / kFastSampleBatch);
        }
        result.faults_after = read_page_faults();

        const auto elapsed = end - start;
        result.iterations = iterations;
        result.elapsed_seconds = std::chrono::duration<double>(elapsed).count();
        result.checksum = checksum;
        result.stats = build_stats(
            std::move(samples),
            std::chrono::duration<double, std::nano>(elapsed).count() /
                static_cast<double>(iterations));
    });
}

TestResult run_heap_test(DWORD cpu,
                         std::uint64_t iterations,
                         std::uint64_t timer_overhead_ns) {
    return run_on_worker("new_delete", cpu, [&](TestResult& result) {
        warm_up_heap();

        std::uint64_t checksum = 0;
        for (std::size_t i = 0; i < 1024; ++i) {
            auto* block = allocate_heap_block();
            *block = {};
            touch_block(block, i, checksum);
            free_heap_block(block);
        }

        std::vector<std::uint64_t> samples;
        samples.reserve(kFastSamples);

        result.faults_before = read_page_faults();

        const auto start = std::chrono::steady_clock::now();
        for (std::uint64_t i = 0; i < iterations; ++i) {
            auto* block = allocate_heap_block();
            touch_block(block, 0xBEEFBEEF00000000ull + i, checksum);
            free_heap_block(block);
        }
        const auto end = std::chrono::steady_clock::now();

        for (std::size_t sample = 0; sample < kFastSamples; ++sample) {
            const std::uint64_t sample_start = now_ns();
            for (std::size_t i = 0; i < kFastSampleBatch; ++i) {
                auto* block = allocate_heap_block();
                touch_block(block, 0xF00D000000000000ull + sample + i,
                            checksum);
                free_heap_block(block);
            }
            const std::uint64_t sample_end = now_ns();
            const std::uint64_t corrected =
                sample_end - sample_start > timer_overhead_ns
                    ? sample_end - sample_start - timer_overhead_ns
                    : 0;
            samples.push_back(corrected / kFastSampleBatch);
        }
        result.faults_after = read_page_faults();

        const auto elapsed = end - start;
        result.iterations = iterations;
        result.elapsed_seconds = std::chrono::duration<double>(elapsed).count();
        result.checksum = checksum;
        result.stats = build_stats(
            std::move(samples),
            std::chrono::duration<double, std::nano>(elapsed).count() /
                static_cast<double>(iterations));
    });
}

TestResult run_virtual_alloc_only_test(DWORD cpu,
                                      std::uint64_t iterations,
                                      std::size_t bytes,
                                      std::uint64_t timer_overhead_ns) {
    return run_on_worker("virtual_alloc_only", cpu, [&](TestResult& result) {
        warm_up_virtual_alloc(bytes);

        std::uint64_t checksum = 0;
        std::vector<std::uint64_t> samples;
        samples.reserve(static_cast<std::size_t>(
            std::min<std::uint64_t>(iterations, kVirtualSamples)));

        result.faults_before = read_page_faults();

        const auto start = std::chrono::steady_clock::now();
        for (std::uint64_t i = 0; i < iterations; ++i) {
            const bool keep_sample = samples.size() < kVirtualSamples;
            const std::uint64_t sample_start = keep_sample ? now_ns() : 0;

            void* region = allocate_virtual_region(bytes);
            if (region == nullptr) {
                throw std::runtime_error("VirtualAlloc failed");
            }
            do_not_optimize(region);
            checksum += reinterpret_cast<std::uintptr_t>(region) & 0xFFu;
            if (!free_virtual_region(region)) {
                throw std::runtime_error("VirtualFree failed");
            }

            if (keep_sample) {
                const std::uint64_t sample_end = now_ns();
                samples.push_back(sample_end - sample_start > timer_overhead_ns
                                      ? sample_end - sample_start -
                                            timer_overhead_ns
                                      : 0);
            }
        }
        const auto end = std::chrono::steady_clock::now();
        result.faults_after = read_page_faults();

        const auto elapsed = end - start;
        result.iterations = iterations;
        result.elapsed_seconds = std::chrono::duration<double>(elapsed).count();
        result.checksum = checksum;
        result.stats = build_stats(
            std::move(samples),
            std::chrono::duration<double, std::nano>(elapsed).count() /
                static_cast<double>(iterations));
    });
}

TestResult run_virtual_alloc_first_touch_test(DWORD cpu,
                                             std::uint64_t iterations,
                                             std::size_t bytes,
                                             std::uint64_t timer_overhead_ns) {
    return run_on_worker("virtual_alloc_first_touch", cpu, [&](TestResult& result) {
        warm_up_virtual_alloc(bytes);

        std::uint64_t checksum = 0;
        std::vector<std::uint64_t> samples;
        samples.reserve(static_cast<std::size_t>(
            std::min<std::uint64_t>(iterations, kVirtualSamples)));

        result.faults_before = read_page_faults();

        const auto start = std::chrono::steady_clock::now();
        for (std::uint64_t i = 0; i < iterations; ++i) {
            const bool keep_sample = samples.size() < kVirtualSamples;
            const std::uint64_t sample_start = keep_sample ? now_ns() : 0;

            void* region = allocate_virtual_region(bytes);
            if (region == nullptr) {
                throw std::runtime_error("VirtualAlloc failed");
            }

            auto* data = static_cast<volatile unsigned char*>(region);
            data[0] = 0x5A;
            checksum += data[0];
            if (!free_virtual_region(region)) {
                throw std::runtime_error("VirtualFree failed");
            }

            if (keep_sample) {
                const std::uint64_t sample_end = now_ns();
                samples.push_back(sample_end - sample_start > timer_overhead_ns
                                      ? sample_end - sample_start -
                                            timer_overhead_ns
                                      : 0);
            }
        }
        const auto end = std::chrono::steady_clock::now();
        result.faults_after = read_page_faults();

        const auto elapsed = end - start;
        result.iterations = iterations;
        result.elapsed_seconds = std::chrono::duration<double>(elapsed).count();
        result.checksum = checksum;
        result.stats = build_stats(
            std::move(samples),
            std::chrono::duration<double, std::nano>(elapsed).count() /
                static_cast<double>(iterations));
    });
}

TestResult run_batch_page_touch_test(DWORD cpu,
                                     std::size_t region_bytes,
                                     std::size_t page_bytes,
                                     std::uint64_t timer_overhead_ns,
                                     bool first_touch) {
    const std::string name =
        first_touch ? "batch_first_touch" : "prefaulted_page_touch";

    return run_on_worker(name, cpu, [&](TestResult& result) {
        const std::size_t pages = region_bytes / page_bytes;
        const std::size_t bytes = pages * page_bytes;

        void* region = allocate_virtual_region(bytes);
        if (region == nullptr) {
            throw std::runtime_error("VirtualAlloc failed for batch region");
        }

        auto* data = static_cast<volatile unsigned char*>(region);
        std::uint64_t checksum = 0;

        if (!first_touch) {
            for (std::size_t i = 0; i < pages; ++i) {
                data[i * page_bytes] = 1;
            }
            clobber_memory();
        }

        std::vector<std::uint64_t> samples;
        samples.reserve(pages);

        result.faults_before = read_page_faults();

        const auto start = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < pages; ++i) {
            const std::size_t offset = i * page_bytes;
            const std::uint64_t sample_start = now_ns();
            data[offset] = static_cast<unsigned char>(first_touch ? 0x33 : 0x44);
            checksum += data[offset];
            const std::uint64_t sample_end = now_ns();
            samples.push_back(sample_end - sample_start > timer_overhead_ns
                                  ? sample_end - sample_start -
                                        timer_overhead_ns
                                  : 0);
        }
        const auto end = std::chrono::steady_clock::now();
        result.faults_after = read_page_faults();

        if (!free_virtual_region(region)) {
            throw std::runtime_error("VirtualFree failed for batch region");
        }

        const auto elapsed = end - start;
        result.iterations = pages;
        result.elapsed_seconds = std::chrono::duration<double>(elapsed).count();
        result.checksum = checksum;
        result.stats = build_stats(
            std::move(samples),
            std::chrono::duration<double, std::nano>(elapsed).count() /
                static_cast<double>(pages));
    });
}

std::vector<KAFFINITY> physical_core_masks() {
    DWORD bytes = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &bytes);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes == 0) {
        return {};
    }

    std::vector<std::byte> buffer(bytes);
    auto* info =
        reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data());
    if (!GetLogicalProcessorInformationEx(RelationProcessorCore, info, &bytes)) {
        return {};
    }

    std::vector<KAFFINITY> masks;
    std::byte* cursor = buffer.data();
    const std::byte* end = buffer.data() + bytes;
    while (cursor < end) {
        auto* entry =
            reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(cursor);
        if (entry->Relationship == RelationProcessorCore) {
            for (WORD i = 0; i < entry->Processor.GroupCount; ++i) {
                masks.push_back(entry->Processor.GroupMask[i].Mask);
            }
        }
        cursor += entry->Size;
    }

    return masks;
}

std::optional<std::size_t> core_index_for_cpu(
    DWORD cpu,
    const std::vector<KAFFINITY>& masks) {
    const KAFFINITY cpu_mask = KAFFINITY{1} << cpu;
    for (std::size_t i = 0; i < masks.size(); ++i) {
        if ((masks[i] & cpu_mask) != 0) {
            return i;
        }
    }
    return std::nullopt;
}

void warn_about_smt_siblings(const CpuSelection& cpus) {
    const auto masks = physical_core_masks();
    if (masks.empty()) {
        std::cout << "warning: cannot inspect physical core topology\n";
        return;
    }

    for (std::size_t i = 0; i < cpus.cpus.size(); ++i) {
        for (std::size_t j = i + 1; j < cpus.cpus.size(); ++j) {
            const auto core_i = core_index_for_cpu(cpus.cpus[i], masks);
            const auto core_j = core_index_for_cpu(cpus.cpus[j], masks);
            if (core_i && core_j && *core_i == *core_j) {
                std::cout << "warning: CPU " << cpus.cpus[i] << " and CPU "
                          << cpus.cpus[j]
                          << " appear to be SMT siblings of one physical core\n";
            }
        }
    }
}

CpuSelection parse_cpu_list(const std::string& value) {
    CpuSelection selection{};
    std::stringstream stream(value);
    std::string item;
    std::size_t index = 0;

    while (std::getline(stream, item, ',')) {
        if (index >= selection.cpus.size()) {
            throw std::invalid_argument("--cpus expects exactly four CPUs");
        }
        selection.cpus[index++] = static_cast<DWORD>(std::stoul(item));
    }

    if (index != selection.cpus.size()) {
        throw std::invalid_argument("--cpus expects exactly four CPUs");
    }

    return selection;
}

Config parse_args(int argc, char** argv) {
    Config config{};

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto split = arg.find('=');
        const std::string key = split == std::string::npos ? arg : arg.substr(0, split);
        const std::string value = split == std::string::npos ? "" : arg.substr(split + 1);

        if (key == "--cpus") {
            config.cpu_selection = parse_cpu_list(value);
        } else if (key == "--pool-iters") {
            config.pool_iterations = std::stoull(value);
        } else if (key == "--heap-iters") {
            config.heap_iterations = std::stoull(value);
        } else if (key == "--virtual-iters") {
            config.virtual_iterations = std::stoull(value);
        } else if (key == "--batch-mib") {
            config.batch_region_bytes =
                static_cast<std::size_t>(std::stoull(value)) * 1024ull * 1024ull;
        } else {
            throw std::invalid_argument(
                "usage: affinity.exe [--cpus=0,2,4,6] [--pool-iters=N] "
                "[--heap-iters=N] [--virtual-iters=N] [--batch-mib=N]");
        }
    }

    return config;
}

void print_result_row(const TestResult& result) {
    const double ops_sec =
        result.elapsed_seconds > 0.0
            ? static_cast<double>(result.iterations) / result.elapsed_seconds
            : 0.0;

    std::cout << std::left << std::setw(27) << result.name << std::right
              << std::setw(12) << result.iterations << std::setw(12)
              << std::fixed << std::setprecision(2) << result.stats.mean_ns
              << std::setw(10) << result.stats.min_ns << std::setw(10)
              << result.stats.p50_ns << std::setw(10) << result.stats.p90_ns
              << std::setw(10) << result.stats.p99_ns << std::setw(12)
              << result.stats.p999_ns << std::setw(12);

    if (result.stats.p9999_ns) {
        std::cout << *result.stats.p9999_ns;
    } else {
        std::cout << "n/a";
    }

    std::cout << std::setw(10) << result.stats.max_ns << std::setw(15)
              << std::fixed << std::setprecision(0) << ops_sec << '\n';
}

void print_details(const TestResult& result) {
    std::cout << result.name << ": " << affinity_text(result.affinity)
              << ", checksum=" << result.checksum
              << ", page_faults_before=" << result.faults_before.page_faults
              << ", page_faults_after=" << result.faults_after.page_faults
              << ", page_faults_delta="
              << result.faults_after.page_faults -
                     result.faults_before.page_faults
              << '\n';
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const Config config = parse_args(argc, argv);
        const std::size_t page_bytes = page_size();
        const std::uint64_t timer_overhead_ns = measure_timer_overhead_ns();

        std::cout << "Windows memory allocation benchmark, C++20\n";
        std::cout << "Build for Release/O2. MSVC does not have -O3; /O2 is the closest release optimization mode.\n";
        std::cout << "page size: " << page_bytes << " bytes\n";
        std::cout << "timer overhead estimate: " << timer_overhead_ns << " ns\n";
        std::cout << "selected CPUs: " << config.cpu_selection.cpus[0] << ", "
                  << config.cpu_selection.cpus[1] << ", "
                  << config.cpu_selection.cpus[2] << ", "
                  << config.cpu_selection.cpus[3] << "\n\n";

        warn_about_smt_siblings(config.cpu_selection);

        std::vector<TestResult> results;
        results.reserve(6);

        results.push_back(run_pool_test(config.cpu_selection.cpus[0],
                                        config.pool_iterations,
                                        timer_overhead_ns));
        results.push_back(run_heap_test(config.cpu_selection.cpus[1],
                                        config.heap_iterations,
                                        timer_overhead_ns));
        results.push_back(run_virtual_alloc_only_test(
            config.cpu_selection.cpus[2], config.virtual_iterations, page_bytes,
            timer_overhead_ns));
        results.push_back(run_virtual_alloc_first_touch_test(
            config.cpu_selection.cpus[3], config.virtual_iterations, page_bytes,
            timer_overhead_ns));
        results.push_back(run_batch_page_touch_test(
            config.cpu_selection.cpus[3], config.batch_region_bytes, page_bytes,
            timer_overhead_ns, true));
        results.push_back(run_batch_page_touch_test(
            config.cpu_selection.cpus[3], config.batch_region_bytes, page_bytes,
            timer_overhead_ns, false));

        std::cout << '\n';
        std::cout << std::left << std::setw(27) << "test" << std::right
                  << std::setw(12) << "iterations" << std::setw(12)
                  << "mean_ns" << std::setw(10) << "min_ns" << std::setw(10)
                  << "p50_ns" << std::setw(10) << "p90_ns" << std::setw(10)
                  << "p99_ns" << std::setw(12) << "p99.9_ns"
                  << std::setw(12) << "p99.99_ns" << std::setw(10)
                  << "max_ns" << std::setw(15) << "ops_sec" << '\n';

        for (const TestResult& result : results) {
            print_result_row(result);
        }

        std::cout << "\nDetails:\n";
        for (const TestResult& result : results) {
            print_details(result);
        }

        std::cout << "\nWhat is measured:\n";
        std::cout << "pool: intrusive fixed-size object pool, fully allocated and touched before timing; no OS calls in the loop.\n";
        std::cout << "new_delete: C++ new/delete for a 64-byte object; mostly user-mode heap allocator cost, not direct physical page allocation.\n";
        std::cout << "virtual_alloc_only: VirtualAlloc/VirtualFree for one committed page without touching it; virtual mapping and commit bookkeeping.\n";
        std::cout << "virtual_alloc_first_touch: VirtualAlloc/VirtualFree plus first byte write/read; includes first-touch demand-zero page fault and physical page materialization.\n";
        std::cout << "batch_first_touch: one large VirtualAlloc region, then first byte write/read per page.\n";
        std::cout << "prefaulted_page_touch: second pass style measurement on already materialized pages, without first-touch faults.\n";
        std::cout << "Windows exposes process PageFaultCount here, not Linux-style minor/major fault counters.\n";
        std::cout << "\nsink: " << g_sink.load(std::memory_order_relaxed)
                  << '\n';
    } catch (const std::exception& ex) {
        std::cerr << "error: " << ex.what() << '\n';
        return 1;
    }

    return 0;
}
