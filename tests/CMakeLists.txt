enable_testing()
include(GoogleTest)
find_package(GTest REQUIRED)

# 自动 add_subdirectory 所有带 CMakeLists.txt 的子目录
file(GLOB _test_subdirs
     LIST_DIRECTORIES true
     RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
     CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_SOURCE_DIR}/*)

foreach(_d ${_test_subdirs})
  if(IS_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/${_d}
     AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${_d}/CMakeLists.txt)
    add_subdirectory(${_d})
  endif()
endforeach()