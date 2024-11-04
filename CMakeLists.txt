cmake_minimum_required(VERSION 3.28)
project(OSlab2)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED True)

add_library(lab2 SHARED lab2/lab2.cpp)

add_executable(ema-search-str lab2/ema-search-str.cpp)

target_link_libraries(ema-search-str lab2 rt)

set_target_properties(ema-search-str PROPERTIES
        BUILD_RPATH "\$ORIGIN"
        INSTALL_RPATH "\$ORIGIN"
)

install(TARGETS lab2 ema-search-str
        RUNTIME DESTINATION bin
        LIBRARY DESTINATION lib
)

add_custom_target(run
        COMMAND LD_LIBRARY_PATH=\$<TARGET_FILE_DIR:lab2> ./ema-search-str text.txt ausf 1
        DEPENDS ema-search-str
        WORKING_DIRECTORY \${CMAKE_BINARY_DIR}
)