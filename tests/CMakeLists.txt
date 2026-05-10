enable_testing()
include(GoogleTest)
find_package(GTest REQUIRED)

add_subdirectory(core)
add_subdirectory(pipeline)
add_subdirectory(md)