add_library(fexma_snapshot_demo STATIC
  src/snapshot_format.cpp
  src/snapshot_loader.cpp
  src/snapshot_sink.cpp
)
add_library(fexma::snapshot_demo ALIAS fexma_snapshot_demo)

target_include_directories(fexma_snapshot_demo
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(fexma_snapshot_demo PUBLIC fexma::wal)
target_compile_features(fexma_snapshot_demo PUBLIC cxx_std_20)

function(fexma_snapshot_demo_apply_warnings target_name)
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

fexma_snapshot_demo_apply_warnings(fexma_snapshot_demo)

add_executable(test_snapshot_demo_stateful_pipeline
  tests/test_stateful_pipeline.cpp
)
target_link_libraries(test_snapshot_demo_stateful_pipeline
  PRIVATE
    fexma::snapshot_demo
)

fexma_snapshot_demo_apply_warnings(test_snapshot_demo_stateful_pipeline)

add_test(NAME test_snapshot_demo_stateful_pipeline
         COMMAND test_snapshot_demo_stateful_pipeline)

add_executable(test_snapshot_demo_snapshot_semantics
  tests/test_snapshot_semantics.cpp
)
target_link_libraries(test_snapshot_demo_snapshot_semantics
  PRIVATE
    fexma::snapshot_demo
)

fexma_snapshot_demo_apply_warnings(test_snapshot_demo_snapshot_semantics)

add_test(NAME test_snapshot_demo_snapshot_semantics
         COMMAND test_snapshot_demo_snapshot_semantics)

add_executable(test_snapshot_demo_snapshot_format
  tests/test_snapshot_format.cpp
)
target_link_libraries(test_snapshot_demo_snapshot_format
  PRIVATE
    fexma::snapshot_demo
)
fexma_snapshot_demo_apply_warnings(test_snapshot_demo_snapshot_format)

add_test(NAME test_snapshot_demo_snapshot_format
         COMMAND test_snapshot_demo_snapshot_format)

add_executable(test_snapshot_demo_snapshot_sink
  tests/test_snapshot_sink.cpp
)
target_link_libraries(test_snapshot_demo_snapshot_sink
  PRIVATE
    fexma::snapshot_demo
)
target_include_directories(test_snapshot_demo_snapshot_sink
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
fexma_snapshot_demo_apply_warnings(test_snapshot_demo_snapshot_sink)

add_test(NAME test_snapshot_demo_snapshot_sink
         COMMAND test_snapshot_demo_snapshot_sink)

add_executable(test_snapshot_demo_bootstrap_restore
  tests/test_bootstrap_restore.cpp
)
target_link_libraries(test_snapshot_demo_bootstrap_restore
  PRIVATE
    fexma::snapshot_demo
)
fexma_snapshot_demo_apply_warnings(test_snapshot_demo_bootstrap_restore)

add_test(NAME test_snapshot_demo_bootstrap_restore
         COMMAND test_snapshot_demo_bootstrap_restore)

add_executable(test_snapshot_demo_progress_and_repeated_snapshots
  tests/test_progress_and_repeated_snapshots.cpp
)
target_link_libraries(test_snapshot_demo_progress_and_repeated_snapshots
  PRIVATE
    fexma::snapshot_demo
)
fexma_snapshot_demo_apply_warnings(
  test_snapshot_demo_progress_and_repeated_snapshots)

add_test(NAME test_snapshot_demo_progress_and_repeated_snapshots
         COMMAND test_snapshot_demo_progress_and_repeated_snapshots)

add_executable(test_snapshot_demo_large_capture
  tests/test_large_capture.cpp
)
target_link_libraries(test_snapshot_demo_large_capture
  PRIVATE
    fexma::snapshot_demo
)
fexma_snapshot_demo_apply_warnings(test_snapshot_demo_large_capture)

add_test(NAME test_snapshot_demo_large_capture
         COMMAND test_snapshot_demo_large_capture)
set_tests_properties(test_snapshot_demo_large_capture PROPERTIES
  LABELS stress
  RUN_SERIAL TRUE
  TIMEOUT 300
)
