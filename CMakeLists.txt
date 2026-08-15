cmake_minimum_required(VERSION 3.16)

project(ll_examples LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

enable_testing()

add_executable(tp utilities/tp.cpp)
add_executable(numa utilities/numa.cpp)
add_executable(affinity utilities/affinity.cpp)
add_executable(cache_locality utilities/cache_locality.cpp)
add_subdirectory(exchange/wal)
add_subdirectory(exchange/order_book)
add_subdirectory(exchange/matcher)

find_package(Threads REQUIRED)
target_link_libraries(affinity PRIVATE Threads::Threads)
target_link_libraries(cache_locality PRIVATE Threads::Threads)

if(MSVC)
  target_compile_options(numa PRIVATE
    /W4
    /permissive-
    /EHsc
  )
  target_compile_options(affinity PRIVATE
    /W4
    /permissive-
    /EHsc
    $<$<CONFIG:Release>:/O2>
  )
  target_compile_options(cache_locality PRIVATE
    /W4
    /permissive-
    /EHsc
    $<$<CONFIG:Release>:/O2>
  )
  target_compile_options(tp PRIVATE
    /W4
    /permissive-
    /EHsc
  )
else()
  target_compile_options(numa PRIVATE
    -Wall
    -Wextra
    -Wpedantic
  )
  target_compile_options(affinity PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    $<$<CONFIG:Release>:-O3>
  )
  target_compile_options(cache_locality PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    $<$<CONFIG:Release>:-O3>
  )
  target_compile_options(tp PRIVATE
    -Wall
    -Wextra
    -Wpedantic
  )
endif()

if(WIN32)
  target_compile_definitions(numa PRIVATE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
  )
  target_compile_definitions(affinity PRIVATE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
  )
  target_compile_definitions(cache_locality PRIVATE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
  )
  target_link_libraries(affinity PRIVATE Psapi)
  target_link_libraries(cache_locality PRIVATE Psapi)
else()
  target_compile_definitions(numa PRIVATE
    _GNU_SOURCE
  )
  target_compile_definitions(affinity PRIVATE
    _GNU_SOURCE
  )
  target_compile_definitions(cache_locality PRIVATE
    _GNU_SOURCE
  )

  find_path(NUMA_INCLUDE_DIR numa.h)
  find_library(NUMA_LIBRARY numa)

  if(NUMA_INCLUDE_DIR AND NUMA_LIBRARY)
    target_include_directories(numa PRIVATE ${NUMA_INCLUDE_DIR})
    target_link_libraries(numa PRIVATE ${NUMA_LIBRARY})
  else()
    message(WARNING "libnuma was not found. Install libnuma-dev/numactl-devel if numa.cpp uses NUMA APIs.")
  endif()
endif()
