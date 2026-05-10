add_executable(test_core
  test_types.cpp
  test_clock.cpp
  test_spsc_ring.cpp
  test_order_book.cpp
)
find_package(Threads REQUIRED)
target_link_libraries(test_core
  PRIVATE
    hft_core
    GTest::gtest_main
    Threads::Threads
)
include(GoogleTest)
gtest_discover_tests(test_core)