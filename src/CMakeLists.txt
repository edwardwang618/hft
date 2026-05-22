find_package(Threads REQUIRED)

add_executable(hft main.cpp)

target_link_libraries(hft
    PRIVATE
        hft_core
        Threads::Threads
)
