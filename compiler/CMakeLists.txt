cmake_minimum_required(VERSION 3.20)
project(kinic CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(kinic
    src/main.cpp
    src/lexer.cpp
    src/parser.cpp
    src/sema.cpp
    src/driver.cpp
    src/optimizer.cpp
    src/codegen.cpp
    src/diagnostic.cpp
    src/lsp.cpp

    src/fmt.cpp
)
target_include_directories(kinic PRIVATE include)

if(MSVC)
    target_compile_options(kinic PRIVATE /utf-8 /W4)
else()
    target_compile_options(kinic PRIVATE -Wall -Wextra -Wpedantic)
endif()
