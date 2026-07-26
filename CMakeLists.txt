cmake_minimum_required(VERSION 3.16)

project(ll_examples LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(numa numa.cpp)

if(MSVC)
  target_compile_options(numa PRIVATE
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
endif()

if(WIN32)
  target_compile_definitions(numa PRIVATE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
  )
else()
  target_compile_definitions(numa PRIVATE
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
