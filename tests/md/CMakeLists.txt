# tests/md/CMakeLists.txt

add_executable(test_binary_parser
  test_binary_parser.cpp
  test_feed_handler.cpp
)

target_link_libraries(test_binary_parser
  PRIVATE
    hft_core
    GTest::gtest
    GTest::gtest_main
)

target_include_directories(test_binary_parser
  PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)

gtest_discover_tests(test_binary_parser)