# tests/pipeline/CMakeLists.txt

add_executable(test_book_builder
  test_book_builder.cpp
)

target_link_libraries(test_book_builder
  PRIVATE
    hft_core          
    GTest::gtest
    GTest::gtest_main
)

target_include_directories(test_book_builder
  PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)

gtest_discover_tests(test_book_builder)