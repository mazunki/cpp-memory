cmake_minimum_required(VERSION 3.16)
project(cpp_memory LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(cpp_memory
    main.cpp
    api/mem/buddy.hpp
)

target_include_directories(cpp_memory PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/api
)

target_compile_options(cpp_memory PRIVATE
  $<$<CONFIG:Debug>:-O1 -g3 -fno-omit-frame-pointer -fsanitize=address,undefined>
)
target_link_options(cpp_memory PRIVATE
  $<$<CONFIG:Debug>:-fsanitize=address,undefined>
)

