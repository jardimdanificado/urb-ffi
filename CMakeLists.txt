cmake_minimum_required(VERSION 3.16)
project(urbc C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

find_path(LIBFFI_INCLUDE_DIR ffi.h)
find_library(LIBFFI_LIBRARY NAMES ffi libffi)

if(NOT LIBFFI_INCLUDE_DIR OR NOT LIBFFI_LIBRARY)
    message(FATAL_ERROR "libffi not found. Set LIBFFI_INCLUDE_DIR and LIBFFI_LIBRARY.")
endif()

set(URBC_SOURCES
    src/urbc_format.c
    src/urbc_platform.c
    src/urbc_api.c
    src/urbc_ffi_sig.c
    src/urbc_loader.c
    src/urbc_schema.c
    src/urbc_runtime.c
    src/urbc_ops_core.c
    src/urbc_ops_mem.c
    src/urbc_ops_schema.c
    src/urbc_ops_ffi.c
)

add_library(urbc STATIC ${URBC_SOURCES})
target_include_directories(urbc
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LIBFFI_INCLUDE_DIR}
)
target_link_libraries(urbc PUBLIC ${LIBFFI_LIBRARY})

if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(urbc PUBLIC Threads::Threads dl)
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/urbccli.c)
    add_executable(urbccli urbccli.c)
    target_include_directories(urbccli PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LIBFFI_INCLUDE_DIR}
    )
    target_link_libraries(urbccli PRIVATE ${LIBFFI_LIBRARY})
    if(NOT WIN32)
        target_link_libraries(urbccli PRIVATE Threads::Threads dl)
    endif()
endif()
