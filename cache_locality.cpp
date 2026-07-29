#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#error cache_locality.cpp is a Windows/MSVC benchmark.
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

constexpr std::uint64_t KiB = 1024;
constexpr std::uint64_t MiB = 1024 * KiB;
constexpr std::uint32_t kSpinPauseIters = 64;
constexpr std::size_t kDefaultCacheRounds = 24;
constexpr std::size_t kDefaultHandoffRounds = 80;
constexpr std::size_t kDefaultMigrationRounds = 60;
constexpr std::size_t kFalseSharingBatches = 5000;
constexpr std::size_t kFalseSharingBatchIncrements = 1024;
constexpr std::uint64_t kFixedSeed = 0xC001C0DE12345678ull;

std::atomic<std::uint64_t> g_sink{0};

void compiler_barrier() {
    _ReadWriteBarrier();
    std::atomic_signal_fence(std::memory_order_seq_cst);
}

template <class T>
void do_not_optimize(const T& value) {
    const volatile auto* ptr = &value;
    (void)ptr;
    compiler_barrier();
}

void cpu_relax() {
    for (std::uint32_t i = 0; i < kSpinPauseIters; ++i) {
        _mm_pause();
    }
}

std::string win_error(const char* what) {
    std::ostringstream out;
    out << what << " failed, GetLastError=" << GetLastError();
    return out.str();
}

void print_win_error(const char* what) {
    std::cerr << win_error(what) << '\n';
}

std::string bytes_text(std::uint64_t bytes) {
    std::ostringstream out;
    if (bytes % MiB == 0) {
        out << (bytes / MiB) << " MiB";
    } else if (bytes % KiB == 0) {
        out << (bytes / KiB) << " KiB";
    } else {
        out << bytes << " B";
    }
    return out.str();
}

struct HighResolutionTimer {
    LARGE_INTEGER frequency{};

    HighResolutionTimer() {
        if (!QueryPerformanceFrequency(&frequency)) {
            print_win_error("QueryPerformanceFrequency");
            frequency.QuadPart = 1;
        }
    }

    std::uint64_t ticks() const {
        LARGE_INTEGER value{};
        QueryPerformanceCounter(&value);
        return static_cast<std::uint64_t>(value.QuadPart);
    }

    double ns(std::uint64_t begin, std::uint64_t end) const {
        return static_cast<double>(end - begin) * 1'000'000'000.0 /
               static_cast<double>(frequency.QuadPart);
    }
};

struct SampleStatistics {
    double mean = 0.0;
    std::uint64_t min = 0;
    std::uint64_t p50 = 0;
    std::uint64_t p90 = 0;
    std::uint64_t p99 = 0;
    std::uint64_t p999 = 0;
    std::optional<std::uint64_t> p9999;
    std::uint64_t max = 0;

    static std::uint64_t percentile(const std::vector<std::uint64_t>& sorted,
                                    double p) {
        if (sorted.empty()) {
            return 0;
        }
        const double idx = p * static_cast<double>(sorted.size() - 1);
        return sorted[static_cast<std::size_t>(idx + 0.5)];
    }

    static SampleStatistics from(std::vector<std::uint64_t> samples) {
        SampleStatistics s{};
        if (samples.empty()) {
            return s;
        }

        const long double total =
            std::accumulate(samples.begin(), samples.end(), 0.0L);
        s.mean = static_cast<double>(total / samples.size());

        std::sort(samples.begin(), samples.end());
        s.min = samples.front();
        s.p50 = percentile(samples, 0.50);
        s.p90 = percentile(samples, 0.90);
        s.p99 = percentile(samples, 0.99);
        s.p999 = percentile(samples, 0.999);
        if (samples.size() >= 10'000) {
            s.p9999 = percentile(samples, 0.9999);
        }
        s.max = samples.back();
        return s;
    }
};

struct PageFaultSnapshot {
    DWORD faults = 0;
};

PageFaultSnapshot read_page_faults() {
    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof(counters);
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
        print_win_error("GetProcessMemoryInfo");
        return {};
    }
    return {counters.PageFaultCount};
}

struct VirtualRegion {
    std::uint8_t* data = nullptr;
    std::size_t size = 0;

    VirtualRegion() = default;
    explicit VirtualRegion(std::size_t bytes) { allocate(bytes); }

    VirtualRegion(const VirtualRegion&) = delete;
    VirtualRegion& operator=(const VirtualRegion&) = delete;

    VirtualRegion(VirtualRegion&& other) noexcept
        : data(other.data), size(other.size) {
        other.data = nullptr;
        other.size = 0;
    }

    VirtualRegion& operator=(VirtualRegion&& other) noexcept {
        if (this != &other) {
            release();
            data = other.data;
            size = other.size;
            other.data = nullptr;
            other.size = 0;
        }
        return *this;
    }

    ~VirtualRegion() { release(); }

    bool allocate(std::size_t bytes) {
        release();
        data = static_cast<std::uint8_t*>(
            VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        size = data == nullptr ? 0 : bytes;
        if (data == nullptr) {
            print_win_error("VirtualAlloc");
            return false;
        }
        return true;
    }

    void release() {
        if (data != nullptr) {
            if (!VirtualFree(data, 0, MEM_RELEASE)) {
                print_win_error("VirtualFree");
            }
            data = nullptr;
            size = 0;
        }
    }

    void prefault(std::size_t page_size) {
        for (std::size_t off = 0; off < size; off += page_size) {
            data[off] = static_cast<std::uint8_t>(off);
        }
        compiler_barrier();
    }
};

struct CpuTopology {
    std::size_t page_size = 4096;
    DWORD cpu_count = 1;
    DWORD numa_nodes = 1;
    std::uint32_t cache_line_size = 64;
    std::vector<std::uint64_t> l1_data_sizes;
    std::vector<std::uint64_t> l2_sizes;
    std::vector<std::uint64_t> l3_sizes;
    std::vector<std::uint32_t> associativity;

    static CpuTopology query() {
        CpuTopology topo{};
        SYSTEM_INFO info{};
        GetSystemInfo(&info);
        topo.page_size = info.dwPageSize;
        topo.cpu_count = info.dwNumberOfProcessors;

        ULONG highest_node = 0;
        if (GetNumaHighestNodeNumber(&highest_node)) {
            topo.numa_nodes = highest_node + 1;
        }

        DWORD bytes = 0;
        GetLogicalProcessorInformationEx(RelationCache, nullptr, &bytes);
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes == 0) {
            return topo;
        }

        std::vector<std::byte> buffer(bytes);
        auto* first = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(
            buffer.data());
        if (!GetLogicalProcessorInformationEx(RelationCache, first, &bytes)) {
            print_win_error("GetLogicalProcessorInformationEx(RelationCache)");
            return topo;
        }

        for (std::byte* p = buffer.data(); p < buffer.data() + bytes;) {
            auto* entry =
                reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(p);
            if (entry->Relationship == RelationCache) {
                const auto& cache = entry->Cache;
                topo.cache_line_size =
                    std::max<std::uint32_t>(topo.cache_line_size, cache.LineSize);
                topo.associativity.push_back(cache.Associativity);
                if (cache.Level == 1 && cache.Type == CacheData) {
                    topo.l1_data_sizes.push_back(cache.CacheSize);
                } else if (cache.Level == 2) {
                    topo.l2_sizes.push_back(cache.CacheSize);
                } else if (cache.Level == 3) {
                    topo.l3_sizes.push_back(cache.CacheSize);
                }
            }
            p += entry->Size;
        }
        return topo;
    }

    std::uint64_t llc_size_or_default() const {
        if (!l3_sizes.empty()) {
            return *std::max_element(l3_sizes.begin(), l3_sizes.end());
        }
        return 16 * MiB;
    }

    void print() const {
        std::cout << "CPU topology:\n";
        std::cout << "  logical CPUs: " << cpu_count << '\n';
        std::cout << "  NUMA nodes reported by Windows: " << numa_nodes << '\n';
        std::cout << "  page size: " << page_size << " bytes\n";
        std::cout << "  cache line size: " << cache_line_size << " bytes\n";
        auto print_sizes = [](const char* name,
                              const std::vector<std::uint64_t>& sizes) {
            std::cout << "  " << name << ": ";
            if (sizes.empty()) {
                std::cout << "unknown\n";
                return;
            }
            std::vector<std::uint64_t> unique = sizes;
            std::sort(unique.begin(), unique.end());
            unique.erase(std::unique(unique.begin(), unique.end()), unique.end());
            for (std::size_t i = 0; i < unique.size(); ++i) {
                if (i != 0) {
                    std::cout << ", ";
                }
                std::cout << bytes_text(unique[i]);
            }
            std::cout << '\n';
        };
        print_sizes("L1 data cache", l1_data_sizes);
        print_sizes("L2 cache", l2_sizes);
        print_sizes("L3 cache", l3_sizes);
        if (!associativity.empty()) {
            std::cout << "  associativity values:";
            for (std::size_t i = 0; i < std::min<std::size_t>(associativity.size(), 8);
                 ++i) {
                std::cout << ' ' << associativity[i];
            }
            std::cout << "\n\n";
        }
    }
};

struct AffinityState {
    DWORD requested_cpu = 0;
    DWORD start_cpu = MAXDWORD;
    DWORD end_cpu = MAXDWORD;
    bool pinned = false;
    bool migrated = false;
};

class AffinityGuard {
public:
    explicit AffinityGuard(DWORD cpu) : cpu_(cpu) {
        previous_mask_ =
            SetThreadAffinityMask(GetCurrentThread(), DWORD_PTR{1} << cpu);
        pinned_ = previous_mask_ != 0;
        if (!pinned_) {
            print_win_error("SetThreadAffinityMask");
        }
        wait_until_on_cpu(cpu);
    }

    AffinityGuard(const AffinityGuard&) = delete;
    AffinityGuard& operator=(const AffinityGuard&) = delete;

    ~AffinityGuard() {
        if (previous_mask_ != 0) {
            SetThreadAffinityMask(GetCurrentThread(), previous_mask_);
        }
    }

    bool pinned() const { return pinned_; }
    DWORD cpu() const { return cpu_; }

    static bool set_current_cpu(DWORD cpu) {
        if (SetThreadAffinityMask(GetCurrentThread(), DWORD_PTR{1} << cpu) == 0) {
            print_win_error("SetThreadAffinityMask");
            return false;
        }
        return wait_until_on_cpu(cpu);
    }

    static bool wait_until_on_cpu(DWORD cpu) {
        for (std::size_t i = 0; i < 10000; ++i) {
            if (GetCurrentProcessorNumber() == cpu) {
                return true;
            }
            Sleep(0);
        }
        return GetCurrentProcessorNumber() == cpu;
    }

private:
    DWORD cpu_ = 0;
    DWORD_PTR previous_mask_ = 0;
    bool pinned_ = false;
};

AffinityState begin_affinity(DWORD cpu) {
    AffinityState s{};
    s.requested_cpu = cpu;
    s.pinned = AffinityGuard::set_current_cpu(cpu);
    s.start_cpu = GetCurrentProcessorNumber();
    return s;
}

void end_affinity(AffinityState& s) {
    s.end_cpu = GetCurrentProcessorNumber();
    s.migrated = s.start_cpu != s.end_cpu;
}

struct Config {
    DWORD cpu_a = 0;
    DWORD cpu_b = 2;
    bool quick = false;
};

Config parse_args(int argc, char** argv) {
    Config cfg{};
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto pos = arg.find('=');
        std::string key = pos == std::string::npos ? arg : arg.substr(0, pos);
        std::string value = pos == std::string::npos ? "" : arg.substr(pos + 1);
        if (key == "--cpu-a") {
            cfg.cpu_a = static_cast<DWORD>(std::stoul(value));
        } else if (key == "--cpu-b") {
            cfg.cpu_b = static_cast<DWORD>(std::stoul(value));
        } else if (key == "--quick") {
            cfg.quick = true;
        } else {
            std::cerr << "usage: cache_locality.exe [--cpu-a=N] [--cpu-b=N] [--quick]\n";
        }
    }
    return cfg;
}

std::vector<std::uint64_t> default_working_sets(bool quick) {
    if (quick) {
        return {32 * KiB, 256 * KiB, 2 * MiB, 16 * MiB};
    }
    return {32 * KiB, 256 * KiB, 2 * MiB, 16 * MiB, 64 * MiB};
}

std::vector<std::uint64_t> first_touch_sizes(bool quick) {
    if (quick) {
        return {4 * MiB, 64 * MiB};
    }
    return {4 * MiB, 64 * MiB, 256 * MiB};
}

void pause_between_tests() {
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
}

std::vector<std::size_t> make_pointer_cycle(std::size_t nodes) {
    std::vector<std::size_t> order(nodes);
    std::iota(order.begin(), order.end(), 0);
    std::uint64_t state = kFixedSeed + nodes;
    for (std::size_t i = nodes - 1; i > 0; --i) {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        std::swap(order[i], order[state % (i + 1)]);
    }
    std::vector<std::size_t> next(nodes);
    for (std::size_t i = 0; i < nodes; ++i) {
        next[order[i]] = order[(i + 1) % nodes];
    }
    return next;
}

std::uint64_t sequential_read_pass(const std::uint8_t* data,
                                   std::size_t bytes,
                                   std::size_t stride) {
    std::uint64_t checksum = 0;
    for (std::size_t off = 0; off < bytes; off += stride) {
        checksum += data[off];
    }
    do_not_optimize(checksum);
    return checksum;
}

std::uint64_t sequential_write_pass(std::uint8_t* data,
                                    std::size_t bytes,
                                    std::size_t stride,
                                    std::uint8_t value) {
    std::uint64_t checksum = 0;
    for (std::size_t off = 0; off < bytes; off += stride) {
        data[off] = static_cast<std::uint8_t>(value + off);
        checksum += data[off];
    }
    do_not_optimize(checksum);
    return checksum;
}

std::uint64_t pointer_chase_pass(const std::vector<std::size_t>& next,
                                 std::size_t steps,
                                 std::size_t& index) {
    for (std::size_t i = 0; i < steps; ++i) {
        index = next[index];
    }
    do_not_optimize(index);
    return index;
}

double gib_per_second(std::uint64_t bytes, double ns) {
    if (ns <= 0.0) {
        return 0.0;
    }
    const double gib = static_cast<double>(bytes) / static_cast<double>(1ull << 30);
    return gib / (ns / 1'000'000'000.0);
}

struct TimedPass {
    double ns = 0.0;
    std::uint64_t checksum = 0;
};

template <class Fn>
TimedPass measure_pass(const HighResolutionTimer& timer, Fn&& fn) {
    compiler_barrier();
    const auto begin = timer.ticks();
    const std::uint64_t checksum = fn();
    const auto end = timer.ticks();
    compiler_barrier();
    return {timer.ns(begin, end), checksum};
}

void evict_caches(VirtualRegion& eviction, std::size_t stride) {
    if (eviction.data == nullptr) {
        return;
    }
    g_sink.fetch_add(sequential_read_pass(eviction.data, eviction.size, stride),
                     std::memory_order_relaxed);
}

void initialize_cache_lines(VirtualRegion& region, std::size_t stride) {
    for (std::size_t off = 0; off < region.size; off += stride) {
        region.data[off] = static_cast<std::uint8_t>((off / stride) * 13 + 7);
    }
    compiler_barrier();
}

void print_stats_columns(const SampleStatistics& s) {
    std::cout << std::fixed << std::setprecision(1) << std::setw(11) << s.mean
              << std::setprecision(0) << std::setw(9) << s.min << std::setw(9)
              << s.p50 << std::setw(9) << s.p90 << std::setw(9) << s.p99
              << std::setw(10) << s.p999 << std::setw(10);
    if (s.p9999) {
        std::cout << *s.p9999;
    } else {
        std::cout << "n/a";
    }
    std::cout << std::setw(9) << s.max;
}

struct CacheRow {
    std::uint64_t working_set = 0;
    std::string pattern;
    SampleStatistics cold;
    SampleStatistics hot;
    double ratio = 0.0;
    double bandwidth_gib = 0.0;
    std::uint64_t accesses = 0;
    std::uint64_t checksum = 0;
    AffinityState affinity{};
};

struct HandoffRow {
    std::uint64_t working_set = 0;
    std::string mode;
    SampleStatistics first;
    SampleStatistics second;
    double ratio = 0.0;
    std::uint64_t checksum = 0;
};

struct FirstTouchRow {
    std::uint64_t region_size = 0;
    std::size_t pages = 0;
    std::string pass;
    DWORD faults_delta = 0;
    SampleStatistics stats;
    double total_ms = 0.0;
    std::uint64_t checksum = 0;
    AffinityState affinity{};
};

struct MigrationRow {
    std::uint64_t working_set = 0;
    SampleStatistics baseline_a;
    SampleStatistics first_b;
    SampleStatistics second_b;
    SampleStatistics stable_b;
    SampleStatistics first_a_return;
    SampleStatistics stable_a_return;
    double first_ratio = 0.0;
    double stable_ratio = 0.0;
    std::uint64_t checksum = 0;
};

struct FalseSharingRow {
    std::string layout;
    double total_ops_sec = 0.0;
    double ns_per_increment = 0.0;
    SampleStatistics batch_stats;
    std::uint64_t value_a = 0;
    std::uint64_t value_b = 0;
};

}  // namespace

int main(int argc, char** argv);

class CacheLocalityBenchmark {
public:
    CacheLocalityBenchmark(const CpuTopology& topo, const Config& cfg)
        : topo_(topo), cfg_(cfg), timer_() {}

    std::vector<CacheRow> run() {
        std::vector<CacheRow> rows;
        const auto sets = default_working_sets(cfg_.quick);
        const std::size_t rounds = cfg_.quick ? 10 : kDefaultCacheRounds;
        const std::size_t stride = topo_.cache_line_size;
        VirtualRegion eviction(static_cast<std::size_t>(topo_.llc_size_or_default() * 4));
        eviction.prefault(topo_.page_size);

        AffinityGuard guard(cfg_.cpu_a);

        for (auto bytes : sets) {
            VirtualRegion region(static_cast<std::size_t>(bytes));
            if (region.data == nullptr) {
                continue;
            }
            region.prefault(topo_.page_size);
            initialize_cache_lines(region, stride);
            const auto nodes =
                std::max<std::size_t>(1, static_cast<std::size_t>(bytes / stride));
            auto next = make_pointer_cycle(nodes);

            rows.push_back(run_sequential(region, eviction, bytes, stride, rounds));
            rows.push_back(run_pointer_chase(next, eviction, bytes, stride, rounds));
        }
        return rows;
    }

private:
    CacheRow run_sequential(VirtualRegion& region,
                            VirtualRegion& eviction,
                            std::uint64_t bytes,
                            std::size_t stride,
                            std::size_t rounds) {
        CacheRow row{};
        row.working_set = bytes;
        row.pattern = "sequential";
        row.accesses = bytes / stride;
        row.affinity = begin_affinity(cfg_.cpu_a);

        std::vector<std::uint64_t> cold;
        std::vector<std::uint64_t> hot;
        cold.reserve(rounds);
        hot.reserve(rounds * 3);

        std::uint64_t checksum = 0;
        for (std::size_t r = 0; r < rounds; ++r) {
            evict_caches(eviction, stride);
            auto c = measure_pass(timer_, [&] {
                return sequential_read_pass(region.data, region.size, stride);
            });
            auto h1 = measure_pass(timer_, [&] {
                return sequential_read_pass(region.data, region.size, stride);
            });
            auto h2 = measure_pass(timer_, [&] {
                return sequential_read_pass(region.data, region.size, stride);
            });
            auto h3 = measure_pass(timer_, [&] {
                return sequential_read_pass(region.data, region.size, stride);
            });
            cold.push_back(static_cast<std::uint64_t>(c.ns / row.accesses));
            hot.push_back(static_cast<std::uint64_t>(h1.ns / row.accesses));
            hot.push_back(static_cast<std::uint64_t>(h2.ns / row.accesses));
            hot.push_back(static_cast<std::uint64_t>(h3.ns / row.accesses));
            checksum += c.checksum + h1.checksum + h2.checksum + h3.checksum;
        }

        end_affinity(row.affinity);
        row.cold = SampleStatistics::from(std::move(cold));
        row.hot = SampleStatistics::from(std::move(hot));
        row.ratio = row.hot.mean > 0.0 ? row.cold.mean / row.hot.mean : 0.0;
        row.bandwidth_gib = gib_per_second(bytes, row.hot.mean * row.accesses);
        row.checksum = checksum;
        g_sink.fetch_add(checksum, std::memory_order_relaxed);
        return row;
    }

    CacheRow run_pointer_chase(std::vector<std::size_t>& next,
                               VirtualRegion& eviction,
                               std::uint64_t bytes,
                               std::size_t stride,
                               std::size_t rounds) {
        CacheRow row{};
        row.working_set = bytes;
        row.pattern = "pointer_chase";
        row.accesses = std::max<std::uint64_t>(1, bytes / stride);
        row.affinity = begin_affinity(cfg_.cpu_a);

        std::vector<std::uint64_t> cold;
        std::vector<std::uint64_t> hot;
        cold.reserve(rounds);
        hot.reserve(rounds * 3);

        std::size_t index = 0;
        std::uint64_t checksum = 0;
        for (std::size_t r = 0; r < rounds; ++r) {
            evict_caches(eviction, stride);
            auto c = measure_pass(timer_, [&] {
                return pointer_chase_pass(next, row.accesses, index);
            });
            auto h1 = measure_pass(timer_, [&] {
                return pointer_chase_pass(next, row.accesses, index);
            });
            auto h2 = measure_pass(timer_, [&] {
                return pointer_chase_pass(next, row.accesses, index);
            });
            auto h3 = measure_pass(timer_, [&] {
                return pointer_chase_pass(next, row.accesses, index);
            });
            cold.push_back(static_cast<std::uint64_t>(c.ns / row.accesses));
            hot.push_back(static_cast<std::uint64_t>(h1.ns / row.accesses));
            hot.push_back(static_cast<std::uint64_t>(h2.ns / row.accesses));
            hot.push_back(static_cast<std::uint64_t>(h3.ns / row.accesses));
            checksum += c.checksum + h1.checksum + h2.checksum + h3.checksum;
        }

        end_affinity(row.affinity);
        row.cold = SampleStatistics::from(std::move(cold));
        row.hot = SampleStatistics::from(std::move(hot));
        row.ratio = row.hot.mean > 0.0 ? row.cold.mean / row.hot.mean : 0.0;
        row.bandwidth_gib = 0.0;
        row.checksum = checksum;
        g_sink.fetch_add(checksum, std::memory_order_relaxed);
        return row;
    }

    const CpuTopology& topo_;
    const Config& cfg_;
    HighResolutionTimer timer_;
};

enum class HandoffMode { ReadAfterRead, ReadAfterWrite, WriteAfterWrite };

std::string handoff_mode_name(HandoffMode mode) {
    switch (mode) {
        case HandoffMode::ReadAfterRead:
            return "read_after_read";
        case HandoffMode::ReadAfterWrite:
            return "read_after_write";
        case HandoffMode::WriteAfterWrite:
            return "write_after_write";
    }
    return "unknown";
}

class CoreHandoffBenchmark {
public:
    CoreHandoffBenchmark(const CpuTopology& topo, const Config& cfg)
        : topo_(topo), cfg_(cfg), timer_() {}

    std::vector<HandoffRow> run() {
        std::vector<HandoffRow> rows;
        const std::vector<std::uint64_t> sizes =
            cfg_.quick ? std::vector<std::uint64_t>{32 * KiB, 256 * KiB, 2 * MiB}
                       : std::vector<std::uint64_t>{32 * KiB, 256 * KiB, 2 * MiB,
                                                    16 * MiB};
        for (auto bytes : sizes) {
            for (auto mode : {HandoffMode::ReadAfterRead,
                              HandoffMode::ReadAfterWrite,
                              HandoffMode::WriteAfterWrite}) {
                rows.push_back(run_one(bytes, mode));
            }
        }
        return rows;
    }

private:
    HandoffRow run_one(std::uint64_t bytes, HandoffMode mode) {
        HandoffRow row{};
        row.working_set = bytes;
        row.mode = handoff_mode_name(mode);

        VirtualRegion region(static_cast<std::size_t>(bytes));
        if (region.data == nullptr) {
            return row;
        }
        region.prefault(topo_.page_size);

        const std::size_t rounds = cfg_.quick ? 30 : kDefaultHandoffRounds;
        const std::size_t stride = topo_.cache_line_size;
        initialize_cache_lines(region, stride);
        const std::uint64_t accesses = bytes / stride;
        std::vector<std::uint64_t> first_samples(rounds);
        std::vector<std::uint64_t> second_samples(rounds);
        for (auto& s : first_samples) {
            s = 0;
        }
        for (auto& s : second_samples) {
            s = 0;
        }

        std::atomic<std::uint32_t> phase{0};
        std::atomic<std::uint64_t> checksum{0};

        std::thread a([&] {
            AffinityGuard guard(cfg_.cpu_a);
            for (std::size_t r = 0; r < rounds; ++r) {
                while (phase.load(std::memory_order_acquire) != r * 2) {
                    cpu_relax();
                }
                warm_for_mode(region, stride, mode, true);
                phase.store(static_cast<std::uint32_t>(r * 2 + 1),
                            std::memory_order_release);
            }
        });

        std::thread b([&] {
            AffinityGuard guard(cfg_.cpu_b);
            for (std::size_t r = 0; r < rounds; ++r) {
                while (phase.load(std::memory_order_acquire) != r * 2 + 1) {
                    cpu_relax();
                }
                auto first = measure_consumer(region, stride, mode);
                auto second = measure_consumer(region, stride, mode);
                first_samples[r] =
                    static_cast<std::uint64_t>(first.ns / accesses);
                second_samples[r] =
                    static_cast<std::uint64_t>(second.ns / accesses);
                checksum.fetch_add(first.checksum + second.checksum,
                                   std::memory_order_relaxed);
                phase.store(static_cast<std::uint32_t>((r + 1) * 2),
                            std::memory_order_release);
            }
        });

        a.join();
        b.join();

        row.first = SampleStatistics::from(std::move(first_samples));
        row.second = SampleStatistics::from(std::move(second_samples));
        row.ratio = row.second.mean > 0.0 ? row.first.mean / row.second.mean : 0.0;
        row.checksum = checksum.load(std::memory_order_relaxed);
        g_sink.fetch_add(row.checksum, std::memory_order_relaxed);
        return row;
    }

    void warm_for_mode(VirtualRegion& region,
                       std::size_t stride,
                       HandoffMode mode,
                       bool producer) {
        for (std::size_t i = 0; i < 6; ++i) {
            if (mode == HandoffMode::ReadAfterRead ||
                (mode == HandoffMode::ReadAfterWrite && !producer)) {
                g_sink.fetch_add(
                    sequential_read_pass(region.data, region.size, stride),
                    std::memory_order_relaxed);
            } else {
                g_sink.fetch_add(sequential_write_pass(region.data, region.size,
                                                       stride, 0x31),
                                 std::memory_order_relaxed);
            }
        }
    }

    TimedPass measure_consumer(VirtualRegion& region,
                               std::size_t stride,
                               HandoffMode mode) {
        if (mode == HandoffMode::WriteAfterWrite) {
            return measure_pass(timer_, [&] {
                return sequential_write_pass(region.data, region.size, stride, 0x42);
            });
        }
        return measure_pass(timer_, [&] {
            return sequential_read_pass(region.data, region.size, stride);
        });
    }

    const CpuTopology& topo_;
    const Config& cfg_;
    HighResolutionTimer timer_;
};

class FirstTouchBenchmark {
public:
    FirstTouchBenchmark(const CpuTopology& topo, const Config& cfg)
        : topo_(topo), cfg_(cfg), timer_() {}

    std::vector<FirstTouchRow> run() {
        std::vector<FirstTouchRow> rows;
        AffinityGuard guard(cfg_.cpu_a);

        for (auto bytes : first_touch_sizes(cfg_.quick)) {
            VirtualRegion region(static_cast<std::size_t>(bytes));
            if (region.data == nullptr) {
                continue;
            }
            rows.push_back(measure_touch(region, bytes, "first_touch"));
            rows.push_back(measure_touch(region, bytes, "second_touch"));
            rows.push_back(measure_touch(region, bytes, "third_touch"));
        }
        return rows;
    }

private:
    FirstTouchRow measure_touch(VirtualRegion& region,
                                std::uint64_t bytes,
                                const std::string& pass) {
        FirstTouchRow row{};
        row.region_size = bytes;
        row.pass = pass;
        row.pages = static_cast<std::size_t>(bytes / topo_.page_size);
        row.affinity = begin_affinity(cfg_.cpu_a);

        std::vector<std::uint64_t> samples(row.pages);
        std::fill(samples.begin(), samples.end(), 0);

        const auto before = read_page_faults();
        const auto total_begin = timer_.ticks();
        std::uint64_t checksum = 0;
        for (std::size_t i = 0; i < row.pages; ++i) {
            const std::size_t off = i * topo_.page_size;
            const auto begin = timer_.ticks();
            region.data[off] = static_cast<std::uint8_t>(0x55 + i);
            checksum += region.data[off];
            const auto end = timer_.ticks();
            samples[i] = static_cast<std::uint64_t>(timer_.ns(begin, end));
        }
        const auto total_end = timer_.ticks();
        const auto after = read_page_faults();

        row.faults_delta = after.faults - before.faults;
        row.total_ms = timer_.ns(total_begin, total_end) / 1'000'000.0;
        row.stats = SampleStatistics::from(std::move(samples));
        row.checksum = checksum;
        end_affinity(row.affinity);
        g_sink.fetch_add(checksum, std::memory_order_relaxed);
        return row;
    }

    const CpuTopology& topo_;
    const Config& cfg_;
    HighResolutionTimer timer_;
};

class MigrationBenchmark {
public:
    MigrationBenchmark(const CpuTopology& topo, const Config& cfg)
        : topo_(topo), cfg_(cfg), timer_() {}

    std::vector<MigrationRow> run_migration() {
        std::vector<MigrationRow> rows;
        for (auto bytes : default_working_sets(cfg_.quick)) {
            rows.push_back(run_one(bytes));
        }
        return rows;
    }

    SampleStatistics run_affinity_admin_cost() {
        const std::size_t rounds = cfg_.quick ? 100 : 1000;
        std::vector<std::uint64_t> samples(rounds);
        AffinityGuard::set_current_cpu(cfg_.cpu_a);
        for (std::size_t i = 0; i < rounds; ++i) {
            const DWORD target = i % 2 == 0 ? cfg_.cpu_b : cfg_.cpu_a;
            const auto begin = timer_.ticks();
            AffinityGuard::set_current_cpu(target);
            const auto end = timer_.ticks();
            samples[i] = static_cast<std::uint64_t>(timer_.ns(begin, end));
        }
        AffinityGuard::set_current_cpu(cfg_.cpu_a);
        return SampleStatistics::from(std::move(samples));
    }

    std::vector<CacheRow> run_control_no_migration() {
        std::vector<CacheRow> rows;
        AffinityGuard guard(cfg_.cpu_a);
        const std::size_t stride = topo_.cache_line_size;
        for (auto bytes : default_working_sets(cfg_.quick)) {
            VirtualRegion region(static_cast<std::size_t>(bytes));
            if (region.data == nullptr) {
                continue;
            }
            region.prefault(topo_.page_size);
            initialize_cache_lines(region, stride);
            for (int i = 0; i < 6; ++i) {
                sequential_read_pass(region.data, region.size, stride);
            }
            CacheRow row{};
            row.working_set = bytes;
            row.pattern = "control_no_migration";
            row.accesses = bytes / stride;
            std::vector<std::uint64_t> samples(cfg_.quick ? 20 : 60);
            std::uint64_t checksum = 0;
            for (auto& s : samples) {
                for (std::size_t spin = 0; spin < 2000; ++spin) {
                    _mm_pause();
                }
                auto pass = measure_pass(timer_, [&] {
                    return sequential_read_pass(region.data, region.size, stride);
                });
                s = static_cast<std::uint64_t>(pass.ns / row.accesses);
                checksum += pass.checksum;
            }
            row.hot = SampleStatistics::from(std::move(samples));
            row.checksum = checksum;
            rows.push_back(row);
        }
        return rows;
    }

    std::vector<CacheRow> run_local_eviction() {
        std::vector<CacheRow> rows;
        AffinityGuard guard(cfg_.cpu_a);
        const std::size_t stride = topo_.cache_line_size;
        VirtualRegion eviction(static_cast<std::size_t>(topo_.llc_size_or_default() * 4));
        eviction.prefault(topo_.page_size);
        for (auto bytes : default_working_sets(cfg_.quick)) {
            VirtualRegion region(static_cast<std::size_t>(bytes));
            if (region.data == nullptr) {
                continue;
            }
            region.prefault(topo_.page_size);
            initialize_cache_lines(region, stride);
            CacheRow row{};
            row.working_set = bytes;
            row.pattern = "after_local_eviction";
            row.accesses = bytes / stride;
            std::vector<std::uint64_t> samples(cfg_.quick ? 20 : 60);
            std::uint64_t checksum = 0;
            for (auto& s : samples) {
                for (int i = 0; i < 5; ++i) {
                    sequential_read_pass(region.data, region.size, stride);
                }
                evict_caches(eviction, stride);
                auto pass = measure_pass(timer_, [&] {
                    return sequential_read_pass(region.data, region.size, stride);
                });
                s = static_cast<std::uint64_t>(pass.ns / row.accesses);
                checksum += pass.checksum;
            }
            row.hot = SampleStatistics::from(std::move(samples));
            row.checksum = checksum;
            rows.push_back(row);
        }
        return rows;
    }

private:
    MigrationRow run_one(std::uint64_t bytes) {
        MigrationRow row{};
        row.working_set = bytes;

        VirtualRegion region(static_cast<std::size_t>(bytes));
        if (region.data == nullptr) {
            return row;
        }
        region.prefault(topo_.page_size);

        const std::size_t stride = topo_.cache_line_size;
        initialize_cache_lines(region, stride);
        const std::uint64_t accesses = bytes / stride;
        const std::size_t rounds = cfg_.quick ? 20 : kDefaultMigrationRounds;

        std::vector<std::uint64_t> baseline(rounds);
        std::vector<std::uint64_t> first_b(rounds);
        std::vector<std::uint64_t> second_b(rounds);
        std::vector<std::uint64_t> stable_b(rounds);
        std::vector<std::uint64_t> first_a(rounds);
        std::vector<std::uint64_t> stable_a(rounds);

        std::uint64_t checksum = 0;
        AffinityGuard::set_current_cpu(cfg_.cpu_a);
        for (std::size_t r = 0; r < rounds; ++r) {
            for (int i = 0; i < 8; ++i) {
                sequential_read_pass(region.data, region.size, stride);
            }
            auto base = measure_pass(timer_, [&] {
                return sequential_read_pass(region.data, region.size, stride);
            });
            baseline[r] = static_cast<std::uint64_t>(base.ns / accesses);
            checksum += base.checksum;

            AffinityGuard::set_current_cpu(cfg_.cpu_b);
            auto b1 = measure_pass(timer_, [&] {
                return sequential_read_pass(region.data, region.size, stride);
            });
            auto b2 = measure_pass(timer_, [&] {
                return sequential_read_pass(region.data, region.size, stride);
            });
            auto b3 = measure_pass(timer_, [&] {
                return sequential_read_pass(region.data, region.size, stride);
            });
            first_b[r] = static_cast<std::uint64_t>(b1.ns / accesses);
            second_b[r] = static_cast<std::uint64_t>(b2.ns / accesses);
            stable_b[r] = static_cast<std::uint64_t>(b3.ns / accesses);
            checksum += b1.checksum + b2.checksum + b3.checksum;

            AffinityGuard::set_current_cpu(cfg_.cpu_a);
            auto a1 = measure_pass(timer_, [&] {
                return sequential_read_pass(region.data, region.size, stride);
            });
            auto a2 = measure_pass(timer_, [&] {
                return sequential_read_pass(region.data, region.size, stride);
            });
            first_a[r] = static_cast<std::uint64_t>(a1.ns / accesses);
            stable_a[r] = static_cast<std::uint64_t>(a2.ns / accesses);
            checksum += a1.checksum + a2.checksum;
        }

        row.baseline_a = SampleStatistics::from(std::move(baseline));
        row.first_b = SampleStatistics::from(std::move(first_b));
        row.second_b = SampleStatistics::from(std::move(second_b));
        row.stable_b = SampleStatistics::from(std::move(stable_b));
        row.first_a_return = SampleStatistics::from(std::move(first_a));
        row.stable_a_return = SampleStatistics::from(std::move(stable_a));
        row.first_ratio = row.baseline_a.mean > 0.0
                              ? row.first_b.mean / row.baseline_a.mean
                              : 0.0;
        row.stable_ratio = row.baseline_a.mean > 0.0
                               ? row.stable_b.mean / row.baseline_a.mean
                               : 0.0;
        row.checksum = checksum;
        g_sink.fetch_add(checksum, std::memory_order_relaxed);
        return row;
    }

    const CpuTopology& topo_;
    const Config& cfg_;
    HighResolutionTimer timer_;
};

struct CountersSharedLine {
    std::atomic<std::uint64_t> a{0};
    std::atomic<std::uint64_t> b{0};
};

#pragma warning(push)
#pragma warning(disable : 4324)
struct alignas(64) PaddedCounter {
    std::atomic<std::uint64_t> value{0};
};
#pragma warning(pop)

struct CountersPadded {
    PaddedCounter a;
    PaddedCounter b;
};

class FalseSharingBenchmark {
public:
    FalseSharingBenchmark(const Config& cfg) : cfg_(cfg), timer_() {}

    std::vector<FalseSharingRow> run() {
        return {run_shared(), run_padded()};
    }

private:
    template <class CounterRef>
    void increment_worker(DWORD cpu,
                          CounterRef& counter,
                          std::atomic<std::uint32_t>& ready,
                          std::atomic<bool>& start,
                          std::vector<std::uint64_t>& samples) {
        AffinityGuard guard(cpu);
        ready.fetch_add(1, std::memory_order_release);
        while (!start.load(std::memory_order_acquire)) {
            cpu_relax();
        }
        for (auto& sample : samples) {
            const auto begin = timer_.ticks();
            for (std::size_t i = 0; i < kFalseSharingBatchIncrements; ++i) {
                counter.fetch_add(1, std::memory_order_relaxed);
            }
            const auto end = timer_.ticks();
            sample = static_cast<std::uint64_t>(
                timer_.ns(begin, end) / kFalseSharingBatchIncrements);
        }
    }

    FalseSharingRow run_shared() {
        CountersSharedLine counters{};
        return run_impl("shared_line", counters.a, counters.b, [&] {
            return std::array<std::uint64_t, 2>{counters.a.load(), counters.b.load()};
        });
    }

    FalseSharingRow run_padded() {
        CountersPadded counters{};
        return run_impl("padded_lines", counters.a.value, counters.b.value, [&] {
            return std::array<std::uint64_t, 2>{counters.a.value.load(),
                                               counters.b.value.load()};
        });
    }

    template <class A, class B, class Values>
    FalseSharingRow run_impl(const std::string& name, A& a, B& b, Values values) {
        FalseSharingRow row{};
        row.layout = name;
        std::vector<std::uint64_t> samples_a(kFalseSharingBatches);
        std::vector<std::uint64_t> samples_b(kFalseSharingBatches);
        std::atomic<std::uint32_t> ready{0};
        std::atomic<bool> start{false};

        std::thread ta([&] {
            increment_worker(cfg_.cpu_a, a, ready, start, samples_a);
        });
        std::thread tb([&] {
            increment_worker(cfg_.cpu_b, b, ready, start, samples_b);
        });

        while (ready.load(std::memory_order_acquire) != 2) {
            cpu_relax();
        }
        const auto begin = timer_.ticks();
        start.store(true, std::memory_order_release);
        ta.join();
        tb.join();
        const auto end = timer_.ticks();

        samples_a.insert(samples_a.end(), samples_b.begin(), samples_b.end());
        row.batch_stats = SampleStatistics::from(std::move(samples_a));

        const auto vals = values();
        row.value_a = vals[0];
        row.value_b = vals[1];
        const auto total_ops = static_cast<double>(row.value_a + row.value_b);
        const double total_ns = timer_.ns(begin, end);
        row.total_ops_sec = total_ns > 0.0 ? total_ops / (total_ns / 1e9) : 0.0;
        row.ns_per_increment = total_ops > 0.0 ? total_ns / total_ops : 0.0;
        return row;
    }

    const Config& cfg_;
    HighResolutionTimer timer_;
};

void print_cache_locality(const std::vector<CacheRow>& rows) {
    std::cout << "\nCache locality\n";
    std::cout << std::left << std::setw(13) << "working_set" << std::setw(16)
              << "pattern" << std::right << std::setw(15) << "cold_ns/acc"
              << std::setw(14) << "hot_ns/acc" << std::setw(9) << "ratio"
              << std::setw(13) << "GiB/s" << std::setw(12) << "accesses"
              << "  cpu start/end migrated checksum\n";
    for (const auto& r : rows) {
        std::cout << std::left << std::setw(13) << bytes_text(r.working_set)
                  << std::setw(16) << r.pattern << std::right << std::fixed
                  << std::setprecision(2) << std::setw(15) << r.cold.mean
                  << std::setw(14) << r.hot.mean << std::setw(9) << r.ratio
                  << std::setw(13) << r.bandwidth_gib << std::setw(12)
                  << r.accesses << "  " << r.affinity.requested_cpu << ' '
                  << r.affinity.start_cpu << '/' << r.affinity.end_cpu << ' '
                  << (r.affinity.migrated ? "yes" : "no") << ' '
                  << r.checksum << '\n';
    }
}

void print_cache_stats_detail(const std::vector<CacheRow>& rows) {
    std::cout << "\nCache locality sample detail, ns/access\n";
    std::cout << std::left << std::setw(13) << "working_set" << std::setw(16)
              << "pattern" << std::setw(8) << "state" << std::right
              << std::setw(11) << "mean" << std::setw(9) << "min"
              << std::setw(9) << "p50" << std::setw(9) << "p90"
              << std::setw(9) << "p99" << std::setw(10) << "p99.9"
              << std::setw(10) << "p99.99" << std::setw(9) << "max" << '\n';
    for (const auto& r : rows) {
        for (const auto* item : {&r.cold, &r.hot}) {
            std::cout << std::left << std::setw(13) << bytes_text(r.working_set)
                      << std::setw(16) << r.pattern << std::setw(8)
                      << (item == &r.cold ? "cold" : "hot") << std::right;
            print_stats_columns(*item);
            std::cout << '\n';
        }
    }
}

void print_handoff(const std::vector<HandoffRow>& rows) {
    std::cout << "\nCore handoff. remote_core means another core, not remote NUMA node.\n";
    std::cout << std::left << std::setw(13) << "working_set" << std::setw(20)
              << "mode" << std::right << std::setw(19) << "first_remote_core"
              << std::setw(15) << "second_local" << std::setw(9) << "ratio"
              << std::setw(14) << "checksum" << '\n';
    for (const auto& r : rows) {
        std::cout << std::left << std::setw(13) << bytes_text(r.working_set)
                  << std::setw(20) << r.mode << std::right << std::fixed
                  << std::setprecision(2) << std::setw(19) << r.first.mean
                  << std::setw(15) << r.second.mean << std::setw(9) << r.ratio
                  << std::setw(14) << r.checksum << '\n';
    }
}

void print_first_touch(const std::vector<FirstTouchRow>& rows) {
    std::cout << "\nFirst touch\n";
    std::cout << std::left << std::setw(13) << "region_size" << std::setw(15)
              << "pass" << std::right << std::setw(9) << "pages"
              << std::setw(9) << "faults" << std::setw(14) << "mean_ns/page"
              << std::setw(9) << "p50" << std::setw(9) << "p99"
              << std::setw(10) << "p99.9" << std::setw(9) << "max"
              << std::setw(12) << "total_ms" << "  cpu start/end checksum\n";
    for (const auto& r : rows) {
        std::cout << std::left << std::setw(13) << bytes_text(r.region_size)
                  << std::setw(15) << r.pass << std::right << std::setw(9)
                  << r.pages << std::setw(9) << r.faults_delta << std::fixed
                  << std::setprecision(1) << std::setw(14) << r.stats.mean
                  << std::setprecision(0) << std::setw(9) << r.stats.p50
                  << std::setw(9) << r.stats.p99 << std::setw(10)
                  << r.stats.p999 << std::setw(9) << r.stats.max
                  << std::setprecision(2) << std::setw(12) << r.total_ms
                  << "  " << r.affinity.requested_cpu << ' ' << r.affinity.start_cpu
                  << '/' << r.affinity.end_cpu << ' ' << r.checksum << '\n';
    }
}

void print_migration(const std::vector<MigrationRow>& rows) {
    std::cout << "\nMigration\n";
    std::cout << std::left << std::setw(13) << "working_set" << std::right
              << std::setw(13) << "baseline_A" << std::setw(11) << "first_B"
              << std::setw(11) << "second_B" << std::setw(11) << "stable_B"
              << std::setw(16) << "first_A_return" << std::setw(13)
              << "stable_A" << std::setw(12) << "first/base" << std::setw(12)
              << "stable/base" << '\n';
    for (const auto& r : rows) {
        std::cout << std::left << std::setw(13) << bytes_text(r.working_set)
                  << std::right << std::fixed << std::setprecision(2)
                  << std::setw(13) << r.baseline_a.mean << std::setw(11)
                  << r.first_b.mean << std::setw(11) << r.second_b.mean
                  << std::setw(11) << r.stable_b.mean << std::setw(16)
                  << r.first_a_return.mean << std::setw(13)
                  << r.stable_a_return.mean << std::setw(12)
                  << r.first_ratio << std::setw(12) << r.stable_ratio << '\n';
    }
}

void print_control_eviction(const std::vector<CacheRow>& control,
                            const std::vector<CacheRow>& eviction) {
    std::cout << "\nMigration controls and local eviction, ns/access\n";
    std::cout << std::left << std::setw(13) << "working_set" << std::setw(24)
              << "case" << std::right << std::setw(11) << "mean"
              << std::setw(9) << "p50" << std::setw(9) << "p99"
              << std::setw(9) << "max" << '\n';
    auto print = [](const CacheRow& r) {
        std::cout << std::left << std::setw(13) << bytes_text(r.working_set)
                  << std::setw(24) << r.pattern << std::right << std::fixed
                  << std::setprecision(2) << std::setw(11) << r.hot.mean
                  << std::setprecision(0) << std::setw(9) << r.hot.p50
                  << std::setw(9) << r.hot.p99 << std::setw(9) << r.hot.max
                  << '\n';
    };
    for (const auto& r : control) {
        print(r);
    }
    for (const auto& r : eviction) {
        print(r);
    }
}

void print_false_sharing(const std::vector<FalseSharingRow>& rows) {
    std::cout << "\nFalse sharing\n";
    std::cout << std::left << std::setw(16) << "layout" << std::right
              << std::setw(18) << "total_ops_sec" << std::setw(18)
              << "ns_per_increment" << std::setw(10) << "p50"
              << std::setw(10) << "p99" << std::setw(14) << "value_a"
              << std::setw(14) << "value_b" << '\n';
    for (const auto& r : rows) {
        std::cout << std::left << std::setw(16) << r.layout << std::right
                  << std::fixed << std::setprecision(0) << std::setw(18)
                  << r.total_ops_sec << std::setprecision(2) << std::setw(18)
                  << r.ns_per_increment << std::setprecision(0) << std::setw(10)
                  << r.batch_stats.p50 << std::setw(10) << r.batch_stats.p99
                  << std::setw(14) << r.value_a << std::setw(14) << r.value_b
                  << '\n';
    }
}

void print_affinity_admin(const SampleStatistics& s) {
    std::cout << "\nSetThreadAffinityMask + actual CPU switch administrative cost, ns\n";
    std::cout << "mean=" << std::fixed << std::setprecision(1) << s.mean
              << " p50=" << s.p50 << " p99=" << s.p99
              << " p99.9=" << s.p999 << " max=" << s.max << '\n';
}

void print_interpretation(const CpuTopology& topo) {
    std::cout << "\nInterpretation:\n";
    std::cout << "1. Small repeated working sets are usually served by private caches.\n";
    std::cout << "2. After switching to another core, that core's private caches are cold.\n";
    std::cout << "3. Data may still be in shared LLC, so another core is often cheaper than full RAM access.\n";
    std::cout << "4. After writes from another core, cache coherence and ownership transfer add cost.\n";
    std::cout << "5. First touch includes demand-zero page faults and physical page materialization.\n";
    std::cout << "6. Repeated page passes should have no first-touch faults.\n";
    std::cout << "7. Thread migration damages cache and TLB locality, but on a one-node system it is not remote NUMA access.\n";
    std::cout << "8. False sharing causes cache-line ping-pong even when threads update different variables.\n\n";
    if (topo.numa_nodes == 1) {
        std::cout << "This machine has one NUMA node.\n";
    } else {
        std::cout << "WARNING: Windows reports " << topo.numa_nodes
                  << " NUMA nodes; this benchmark still does not measure local-versus-remote NUMA memory access.\n";
    }
    std::cout << "The benchmark demonstrates cache locality, cache coherence,\n";
    std::cout << "first-touch page faults, and thread migration costs.\n";
    std::cout << "It does not demonstrate local-versus-remote NUMA memory access.\n";
    std::cout << "Sequential throughput and pointer-chasing latency measure different properties; compare them carefully.\n";
}

int main(int argc, char** argv) {
    const Config cfg = parse_args(argc, argv);
    const CpuTopology topo = CpuTopology::query();

    std::cout << "Windows/MSVC cache locality benchmark, C++20\n";
    std::cout << "Build in Release. MSVC uses /O2 for optimized builds; /O3 is not an MSVC option.\n";
    std::cout << "Requested CPUs: A=" << cfg.cpu_a << " B=" << cfg.cpu_b << '\n';
    std::cout << "TSC is not used for primary results; all main timings are QPC-derived nanoseconds.\n";
    if (cfg.quick) {
        std::cout << "Quick mode enabled: fewer rounds and smaller maximum regions.\n";
    }
    topo.print();

    CacheLocalityBenchmark locality(topo, cfg);
    auto locality_rows = locality.run();
    print_cache_locality(locality_rows);
    print_cache_stats_detail(locality_rows);
    pause_between_tests();

    CoreHandoffBenchmark handoff(topo, cfg);
    auto handoff_rows = handoff.run();
    print_handoff(handoff_rows);
    pause_between_tests();

    FalseSharingBenchmark false_sharing(cfg);
    auto false_rows = false_sharing.run();
    print_false_sharing(false_rows);
    pause_between_tests();

    FirstTouchBenchmark first_touch(topo, cfg);
    auto first_touch_rows = first_touch.run();
    print_first_touch(first_touch_rows);
    pause_between_tests();

    MigrationBenchmark migration(topo, cfg);
    auto migration_rows = migration.run_migration();
    print_migration(migration_rows);
    auto admin_cost = migration.run_affinity_admin_cost();
    print_affinity_admin(admin_cost);
    auto control_rows = migration.run_control_no_migration();
    auto eviction_rows = migration.run_local_eviction();
    print_control_eviction(control_rows, eviction_rows);

    print_interpretation(topo);
    std::cout << "\nsink: " << g_sink.load(std::memory_order_relaxed) << '\n';
    return 0;
}
