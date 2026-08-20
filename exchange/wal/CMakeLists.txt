add_library(fexma_wal STATIC
  src/physical_wal_file.cpp
  src/reader.cpp
  src/wal.cpp
)

add_library(fexma::wal ALIAS fexma_wal)

target_include_directories(fexma_wal
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(fexma_wal PUBLIC cxx_std_20)

function(fexma_wal_apply_warnings target_name)
  if(MSVC)
    target_compile_options(${target_name} PRIVATE
      /W4
      /permissive-
      /EHsc
      /FS
      $<$<CONFIG:Release>:/O2>
    )
  else()
    target_compile_options(${target_name} PRIVATE
      -Wall
      -Wextra
      -Wpedantic
      $<$<CONFIG:Release>:-O3>
    )
  endif()
endfunction()

fexma_wal_apply_warnings(fexma_wal)

add_executable(test_wal_frontier_ring tests/test_wal_frontier_ring.cpp)
target_link_libraries(test_wal_frontier_ring PRIVATE fexma::wal)
target_include_directories(test_wal_frontier_ring PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
fexma_wal_apply_warnings(test_wal_frontier_ring)
add_test(NAME test_wal_frontier_ring COMMAND test_wal_frontier_ring)

add_executable(test_wal_reader tests/test_wal_reader.cpp)
target_link_libraries(test_wal_reader PRIVATE fexma::wal)
fexma_wal_apply_warnings(test_wal_reader)
add_test(NAME test_wal_reader COMMAND test_wal_reader)
