add_library(fexma_binary INTERFACE)
add_library(fexma::binary ALIAS fexma_binary)

target_include_directories(fexma_binary
  INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_features(fexma_binary INTERFACE cxx_std_20)

add_executable(test_binary_little_endian tests/test_little_endian.cpp)
target_link_libraries(test_binary_little_endian PRIVATE fexma::binary)

if(MSVC)
  target_compile_options(test_binary_little_endian PRIVATE
    /W4
    /permissive-
    /EHsc
  )
else()
  target_compile_options(test_binary_little_endian PRIVATE
    -Wall
    -Wextra
    -Wpedantic
  )
endif()

add_test(NAME test_binary_little_endian COMMAND test_binary_little_endian)
