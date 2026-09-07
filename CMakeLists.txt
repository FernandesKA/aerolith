cmake_minimum_required(VERSION 3.16)
project(aerolith CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_executable(aerolith
  src/main.cpp
  src/framebuffer.cpp
  src/scd41.cpp
  src/font5x7.cpp
)

target_include_directories(aerolith PRIVATE include)
target_compile_options(aerolith PRIVATE -Wall -Wextra)

install(TARGETS aerolith RUNTIME DESTINATION bin)
