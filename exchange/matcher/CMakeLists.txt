add_library(fexma_matcher INTERFACE)
add_library(fexma::matcher ALIAS fexma_matcher)

target_include_directories(fexma_matcher
  INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(fexma_matcher INTERFACE cxx_std_20)
target_link_libraries(fexma_matcher INTERFACE fexma::order_book)

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

add_executable(test_matcher tests/test_matcher.cpp)
target_link_libraries(test_matcher PRIVATE fexma::matcher)
fexma_matcher_apply_warnings(test_matcher)
add_test(NAME test_matcher COMMAND test_matcher)

add_executable(matcher_basic_usage examples/basic_usage.cpp)
target_link_libraries(matcher_basic_usage PRIVATE fexma::matcher)
fexma_matcher_apply_warnings(matcher_basic_usage)

add_executable(bench_matcher bench/bench_matcher.cpp)
target_link_libraries(bench_matcher PRIVATE fexma::matcher)
fexma_matcher_apply_warnings(bench_matcher)
