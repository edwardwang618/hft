find_package(Threads REQUIRED)

add_executable(test_risk
    test_risk.cpp
)

target_link_libraries(test_risk
    PRIVATE
        hft_core
        GTest::gtest_main
        Threads::Threads
)

include(GoogleTest)
gtest_discover_tests(test_risk)
