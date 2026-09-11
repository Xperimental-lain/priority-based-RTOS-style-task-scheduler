cmake_minimum_required(VERSION 3.16)
project(rtos_scheduler LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

add_library(rtos STATIC src/rtos.c)
target_include_directories(rtos PUBLIC include)
target_compile_options(rtos PRIVATE -Wall -Wextra)

add_executable(demo_c examples/demo_c.c)
target_link_libraries(demo_c PRIVATE rtos)
target_compile_options(demo_c PRIVATE -Wall -Wextra)

add_executable(demo_cpp examples/demo_cpp.cpp)
target_link_libraries(demo_cpp PRIVATE rtos)
target_compile_options(demo_cpp PRIVATE -Wall -Wextra)

include(CTest)
if(BUILD_TESTING)
    add_executable(test_rtos tests/test_rtos.c)
    target_link_libraries(test_rtos PRIVATE rtos)
    target_compile_options(test_rtos PRIVATE -Wall -Wextra)
    add_test(NAME rtos_integration COMMAND test_rtos)
endif()
