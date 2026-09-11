add_library(fexma_wal STATIC
  src/config.cpp
  src/record_tape.cpp
  src/persistence.cpp
  src/physical_wal_file.cpp
  src/reader.cpp
  src/recovery.cpp
  src/wal.cpp
)

add_library(fexma::wal ALIAS fexma_wal)

target_include_directories(fexma_wal
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(fexma_wal PUBLIC cxx_std_20)
target_link_libraries(fexma_wal PRIVATE fexma::binary)

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

add_executable(test_wal_recovery tests/test_wal_recovery.cpp)
target_link_libraries(test_wal_recovery PRIVATE fexma::wal)
target_include_directories(test_wal_recovery PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
fexma_wal_apply_warnings(test_wal_recovery)
add_test(NAME test_wal_recovery COMMAND test_wal_recovery)

add_executable(test_wal_position_view tests/test_wal_position_view.cpp)
target_link_libraries(test_wal_position_view PRIVATE fexma::wal)
fexma_wal_apply_warnings(test_wal_position_view)
add_test(NAME test_wal_position_view COMMAND test_wal_position_view)

add_executable(test_record_tape tests/test_record_tape.cpp)
target_link_libraries(test_record_tape PRIVATE fexma::wal)
fexma_wal_apply_warnings(test_record_tape)
add_test(NAME test_record_tape COMMAND test_record_tape)

add_executable(test_wal_slider tests/test_wal_slider.cpp)
target_link_libraries(test_wal_slider PRIVATE fexma::wal)
fexma_wal_apply_warnings(test_wal_slider)
add_test(NAME test_wal_slider COMMAND test_wal_slider)

add_executable(test_wal_bare_pipeline tests/test_wal_bare_pipeline.cpp)
target_link_libraries(test_wal_bare_pipeline PRIVATE fexma::wal)
fexma_wal_apply_warnings(test_wal_bare_pipeline)
add_test(NAME test_wal_bare_pipeline COMMAND test_wal_bare_pipeline)

add_executable(test_wal_persistence_slider tests/test_wal_persistence_slider.cpp)
target_link_libraries(test_wal_persistence_slider PRIVATE fexma::wal)
target_include_directories(test_wal_persistence_slider PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
fexma_wal_apply_warnings(test_wal_persistence_slider)
add_test(NAME test_wal_persistence_slider COMMAND test_wal_persistence_slider)
