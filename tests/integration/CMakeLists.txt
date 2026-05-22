find_package(Threads REQUIRED)

add_executable(test_end_to_end
    test_end_to_end.cpp
)

target_link_libraries(test_end_to_end
    PRIVATE
        hft_core
        GTest::gtest_main
        Threads::Threads
)

include(GoogleTest)
gtest_discover_tests(test_end_to_end)
