cmake_minimum_required(VERSION 3.25)
project(lab2_project)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(${CMAKE_SOURCE_DIR}/lab2)

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_library(lab2 SHARED lab2/lab2.cpp)

add_executable(ema-search-str lab2/ema-search-str.cpp)
target_link_libraries(ema-search-str lab2 rt)

find_program(CLANG_FORMAT NAMES clang-format)
if(CLANG_FORMAT)
    add_custom_target(format
            COMMAND ${CLANG_FORMAT} -i ${CMAKE_SOURCE_DIR}/lab2/*.cpp ${CMAKE_SOURCE_DIR}/lab2/*.h
            COMMENT "Running clang-format"
    )
endif()

find_program(CLANG_TIDY NAMES clang-tidy)
if(CLANG_TIDY)
    set(CMAKE_CXX_CLANG_TIDY "${CLANG_TIDY};-warnings-as-errors=*")
endif()

add_custom_target(run_test_1
        COMMAND LD_LIBRARY_PATH=${CMAKE_BINARY_DIR}/lib ${CMAKE_BINARY_DIR}/bin/ema-search-str lab2/test.txt ausf 1
        DEPENDS ema-search-str lab2
)

add_custom_target(run_test_2
        COMMAND LD_LIBRARY_PATH=${CMAKE_BINARY_DIR}/lib ${CMAKE_BINARY_DIR}/bin/ema-search-str lab2/test.txt a 1 &
        LD_LIBRARY_PATH=${CMAKE_BINARY_DIR}/lib ${CMAKE_BINARY_DIR}/bin/ema-search-str lab2/test.txt a 1 & wait
        DEPENDS ema-search-str lab2
)

add_custom_target(run_test_3
        COMMAND LD_LIBRARY_PATH=${CMAKE_BINARY_DIR}/lib ${CMAKE_BINARY_DIR}/bin/ema-search-str lab2/test.txt rrrrrrr 10
        DEPENDS ema-search-str lab2
)

add_custom_target(run_test_4
        COMMAND LD_LIBRARY_PATH=${CMAKE_BINARY_DIR}/lib ${CMAKE_BINARY_DIR}/bin/ema-search-str lab2/test.txt aa 1 &
        LD_LIBRARY_PATH=${CMAKE_BINARY_DIR}/lib ${CMAKE_BINARY_DIR}/bin/ema-search-str lab2/test.txt ragdf 1 &
        LD_LIBRARY_PATH=${CMAKE_BINARY_DIR}/lib ${CMAKE_BINARY_DIR}/bin/ema-search-str lab2/test.txt dfvtrdfgh 1 &
        LD_LIBRARY_PATH=${CMAKE_BINARY_DIR}/lib ${CMAKE_BINARY_DIR}/bin/ema-search-str lab2/test.txt gg 1 & wait
        DEPENDS ema-search-str lab2
)
