cmake_minimum_required(VERSION 3.12)
project(lab2_project)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)


add_compile_options(-fsanitize=address)
add_link_options(-fsanitize=address)


add_library(lab2 SHARED lab2/lab2.cpp)
add_executable(ema-search-str lab2/ema-search-str.cpp)
add_executable(stress-test lab2/stress-test.cpp)
target_link_libraries(ema-search-str lab2 rt)
target_link_libraries(stress-test lab2 rt)

enable_testing()


add_test(NAME Test1 COMMAND ema-search-str ${CMAKE_SOURCE_DIR}/lab2/test.txt ergshbewanjrslijgf 1)
add_test(NAME Test2 COMMAND ema-search-str ${CMAKE_SOURCE_DIR}/lab2/test.txt abhedebjkas 3 & ema-search-str ${CMAKE_SOURCE_DIR}/lab2/test.txt dnweojfr 2)
add_test(NAME Test3 COMMAND ema-search-str ${CMAKE_SOURCE_DIR}/lab2/test.txt abhedebj 3 & ema-search-str ${CMAKE_SOURCE_DIR}/lab2/hard-test.txt dnweojfr 10)
add_test(NAME Test4 COMMAND stress-test)
add_test(NAME Test5 COMMAND stress-test & stress-test & stress-test)