add_library(fexma_order_book STATIC
  src/order_book.cpp
)

add_library(fexma::order_book ALIAS fexma_order_book)

target_include_directories(fexma_order_book
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(fexma_order_book PUBLIC cxx_std_20)

if(MSVC)
  target_compile_options(fexma_order_book PRIVATE
    /W4
    /permissive-
    /EHsc
  )
else()
  target_compile_options(fexma_order_book PRIVATE
    -Wall
    -Wextra
    -Wpedantic
  )
endif()

if(WIN32)
  target_compile_definitions(fexma_order_book PRIVATE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
  )
endif()

enable_testing()

function(fexma_order_book_apply_warnings target_name)
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

set(order_book_tests
  test_reference_order_book
  test_order_pool
  test_order_id_index
  test_price_structures
  test_order_book
  test_randomized
)

foreach(test_name IN LISTS order_book_tests)
  add_executable(${test_name} tests/${test_name}.cpp)
  target_link_libraries(${test_name} PRIVATE fexma::order_book)
  fexma_order_book_apply_warnings(${test_name})
  add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()

add_executable(test_order_book_soak tests/test_order_book_soak.cpp)
target_link_libraries(test_order_book_soak PRIVATE fexma::order_book)
if(WIN32)
  target_link_libraries(test_order_book_soak PRIVATE psapi)
endif()
fexma_order_book_apply_warnings(test_order_book_soak)
add_test(NAME test_order_book_soak_quick
  COMMAND test_order_book_soak --mode quick
)

add_executable(bench_order_book bench/bench_order_book.cpp)
target_link_libraries(bench_order_book PRIVATE fexma::order_book)
fexma_order_book_apply_warnings(bench_order_book)

add_executable(bench_order_pool bench/bench_order_pool.cpp)
target_include_directories(bench_order_pool
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_features(bench_order_pool PRIVATE cxx_std_20)
fexma_order_book_apply_warnings(bench_order_pool)

add_executable(bench_order_id_index bench/bench_order_id_index.cpp)
target_include_directories(bench_order_id_index
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_features(bench_order_id_index PRIVATE cxx_std_20)
fexma_order_book_apply_warnings(bench_order_id_index)
