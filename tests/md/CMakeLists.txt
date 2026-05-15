# tests/md/CMakeLists.txt

add_executable(test_binary_parser
  test_binary_parser.cpp
  test_integration_feed_to_book.cpp
)

target_link_libraries(test_binary_parser
  PRIVATE
    hft_test_fixtures
    hft_core
    GTest::gtest
    GTest::gtest_main
)

target_include_directories(test_binary_parser
  PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)

gtest_discover_tests(test_binary_parser)