# tests/pipeline/CMakeLists.txt

add_executable(test_book_builder test_book_builder.cpp)
target_link_libraries(test_book_builder PRIVATE
    hft_core   # 照你原本写的
    GTest::gtest_main
)
gtest_discover_tests(test_book_builder)

add_executable(test_feed_handler test_feed_handler.cpp)
target_link_libraries(test_feed_handler PRIVATE
    hft_test_fixtures
    hft_core
    GTest::gtest_main
)
target_include_directories(test_feed_handler PRIVATE
    ${CMAKE_SOURCE_DIR}/tests/support
)
gtest_discover_tests(test_feed_handler)

add_executable(test_gap_detector test_gap_detector.cpp)
target_link_libraries(test_gap_detector PRIVATE
    hft_core
    GTest::gtest_main
)
gtest_discover_tests(test_gap_detector)

add_executable(test_gap_detector_integration test_gap_detector_integration.cpp)
target_link_libraries(test_gap_detector_integration PRIVATE
    hft_core
    GTest::gtest_main
)
gtest_discover_tests(test_gap_detector_integration)