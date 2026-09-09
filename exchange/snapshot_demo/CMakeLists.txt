add_library(fexma_snapshot_demo INTERFACE)
add_library(fexma::snapshot_demo ALIAS fexma_snapshot_demo)

target_include_directories(fexma_snapshot_demo
  INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(fexma_snapshot_demo INTERFACE fexma::wal)
target_compile_features(fexma_snapshot_demo INTERFACE cxx_std_20)

add_executable(test_snapshot_demo_stateful_pipeline
  tests/test_stateful_pipeline.cpp
)
target_link_libraries(test_snapshot_demo_stateful_pipeline
  PRIVATE
    fexma::snapshot_demo
)

if(MSVC)
  target_compile_options(test_snapshot_demo_stateful_pipeline PRIVATE
    /W4
    /permissive-
    /EHsc
    /FS
    $<$<CONFIG:Release>:/O2>
  )
else()
  target_compile_options(test_snapshot_demo_stateful_pipeline PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    $<$<CONFIG:Release>:-O3>
  )
endif()

add_test(NAME test_snapshot_demo_stateful_pipeline
         COMMAND test_snapshot_demo_stateful_pipeline)
