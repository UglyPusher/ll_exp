find_package(Threads REQUIRED)

add_library(fexma_matcher STATIC
  src/command_pipeline.cpp
  src/codec.cpp
)
add_library(fexma::matcher ALIAS fexma_matcher)

target_include_directories(fexma_matcher
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(fexma_matcher PUBLIC cxx_std_20)
target_link_libraries(fexma_matcher PUBLIC fexma::order_book)

function(fexma_matcher_apply_warnings target_name)
  if(MSVC)
    target_compile_options(${target_name} PRIVATE
      /W4
      /permissive-
      /EHsc
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

fexma_matcher_apply_warnings(fexma_matcher)

add_executable(test_matcher tests/test_matcher.cpp)
target_link_libraries(test_matcher PRIVATE fexma::matcher)
fexma_matcher_apply_warnings(test_matcher)
add_test(NAME test_matcher COMMAND test_matcher)

add_executable(test_matcher_codec tests/test_codec.cpp)
target_link_libraries(test_matcher_codec PRIVATE fexma::matcher)
fexma_matcher_apply_warnings(test_matcher_codec)
add_test(NAME test_matcher_codec COMMAND test_matcher_codec)

add_executable(test_matcher_wal_stream tests/test_wal_stream.cpp)
target_link_libraries(test_matcher_wal_stream PRIVATE fexma::matcher fexma::wal)
fexma_matcher_apply_warnings(test_matcher_wal_stream)
add_test(NAME test_matcher_wal_stream COMMAND test_matcher_wal_stream)

add_executable(test_command_pipeline tests/test_command_pipeline.cpp)
target_link_libraries(test_command_pipeline PRIVATE fexma::matcher Threads::Threads)
fexma_matcher_apply_warnings(test_command_pipeline)
add_test(NAME test_command_pipeline COMMAND test_command_pipeline)

add_executable(matcher_basic_usage examples/basic_usage.cpp)
target_link_libraries(matcher_basic_usage PRIVATE fexma::matcher)
fexma_matcher_apply_warnings(matcher_basic_usage)

add_executable(bench_matcher bench/bench_matcher.cpp)
target_link_libraries(bench_matcher PRIVATE fexma::matcher)
fexma_matcher_apply_warnings(bench_matcher)
