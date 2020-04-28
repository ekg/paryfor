# Specify the minimum version for CMake

cmake_minimum_required(VERSION 3.1)

# Project's name
project(paryfor)
# We build using c++14
set(CMAKE_CXX_STANDARD 14)

#set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# Use all standard-compliant optimizations
set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O3 -mcx16 -g")
set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -mcx16 -g")

# Set the output folder where your program will be created
set(CMAKE_BINARY_DIR ${CMAKE_SOURCE_DIR}/bin)
set(EXECUTABLE_OUTPUT_PATH ${CMAKE_BINARY_DIR})
set(LIBRARY_OUTPUT_PATH ${CMAKE_SOURCE_DIR}/lib)

# The following folder will be included
include_directories("${PROJECT_SOURCE_DIR}")

# set up our target executable and specify its dependencies and includes
add_executable(paryfor-test
  ${CMAKE_SOURCE_DIR}/test.cpp
  )
target_include_directories(paryfor-test PUBLIC
  "${PROJECT_SOURCE_DIR}")
  
target_link_libraries(paryfor-test
  "-latomic"
  Threads::Threads)

# this was hard to track down
# https://stackoverflow.com/questions/35116327/when-g-static-link-pthread-cause-segmentation-fault-why
set(CMAKE_EXE_LINKER_FLAGS "-static -Wl,--whole-archive -lpthread -Wl,--no-whole-archive")

set(CMAKE_BUILD_TYPE Release)



