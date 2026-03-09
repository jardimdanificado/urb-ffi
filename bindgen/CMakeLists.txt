cmake_minimum_required(VERSION 3.16)
project(urb_bindgen LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_path(LIBCLANG_INCLUDE_DIR
    NAMES clang-c/Index.h
    HINTS
        ENV LIBCLANG_INCLUDE_DIR
        ENV LLVM_INCLUDE_DIR
        ENV CLANG_INCLUDE_DIR
    PATH_SUFFIXES include llvm/include clang/include
)

find_library(LIBCLANG_LIBRARY
    NAMES libclang clang libclang-cpp
    HINTS
        ENV LIBCLANG_LIBRARY
        ENV LIBCLANG_LIBRARY_DIR
        ENV LLVM_LIBRARY_DIR
        ENV CLANG_LIBRARY_DIR
    PATH_SUFFIXES lib lib64 llvm/lib clang/lib
)

if(NOT LIBCLANG_INCLUDE_DIR OR NOT LIBCLANG_LIBRARY)
    message(FATAL_ERROR "libclang not found. Set LIBCLANG_INCLUDE_DIR and LIBCLANG_LIBRARY (or LIBCLANG_LIBRARY_DIR).")
endif()

add_executable(urb-bindgen
    src/main.cpp
)

target_include_directories(urb-bindgen PRIVATE ${LIBCLANG_INCLUDE_DIR})
target_link_libraries(urb-bindgen PRIVATE ${LIBCLANG_LIBRARY})

if(MSVC)
    target_compile_options(urb-bindgen PRIVATE /W4)
else()
    target_compile_options(urb-bindgen PRIVATE -Wall -Wextra -Wpedantic)
endif()
