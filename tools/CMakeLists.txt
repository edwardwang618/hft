find_package(Threads REQUIRED)

add_executable(exchange_server exchange_server.cpp)

target_link_libraries(exchange_server
    PRIVATE
        hft_core
        Threads::Threads
)
