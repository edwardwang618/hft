add_executable(test_net
    test_spsc_sink.cpp
    test_udp_receiver.cpp
)
find_package(Threads REQUIRED)
target_link_libraries(test_net
    PRIVATE
        hft_core
        GTest::gtest_main
        Threads::Threads
)
include(GoogleTest)
gtest_discover_tests(test_net)