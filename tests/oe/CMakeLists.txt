find_package(Threads REQUIRED)

add_executable(test_oe
    test_oe_codec.cpp
)

target_link_libraries(test_oe
    PRIVATE
        hft_core
        GTest::gtest_main
        Threads::Threads
)

include(GoogleTest)
gtest_discover_tests(test_oe)
