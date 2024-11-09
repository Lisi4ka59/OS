cmake_minimum_required(VERSION 3.12)
project(lab2_project)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)


add_compile_options(-fsanitize=address)
add_link_options(-fsanitize=address)


add_library(lab2 SHARED lab2/lab2.cpp)
add_executable(ema-search-str lab2/ema-search-str.cpp)
target_link_libraries(ema-search-str lab2 rt)

enable_testing()


add_test(NAME Test1 COMMAND ema-search-str ${CMAKE_SOURCE_DIR}/lab2/test.txt ausf 1)
add_test(NAME Test2 COMMAND ema-search-str ${CMAKE_SOURCE_DIR}/lab2/test.txt a 1)
add_test(NAME Test3 COMMAND ema-search-str ${CMAKE_SOURCE_DIR}/lab2/test.txt rrrrrrr 10)
add_test(NAME Test4 COMMAND ema-search-str ${CMAKE_SOURCE_DIR}/lab2/test.txt aa 1)