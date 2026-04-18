cmake_minimum_required(VERSION 3.20)

project(cwispr)

# Specify C++ 20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Define the executable (the name of the app and the files it needs)
add_executable(cwispr "src/main.cpp" "src/miniaudio.cpp" "src/device_utils.cpp")

target_include_directories(cwispr PRIVATE include extern)