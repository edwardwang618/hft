add_executable(test_mock_exchange
  test_mock_exchange.cpp
)

target_link_libraries(test_mock_exchange
  PRIVATE
    hft_core
    GTest::gtest
    GTest::gtest_main
)

target_include_directories(test_mock_exchange
  PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)

gtest_discover_tests(test_mock_exchange)