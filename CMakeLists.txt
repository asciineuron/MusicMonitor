cmake_minimum_required(VERSION 3.20)

project(MusicMonitor VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

add_subdirectory(src)

# must come after adding the executable above:

# note cannot use gcc with frameworks or dispatch:
target_link_libraries(MusicMonitor PUBLIC "-framework CoreServices")

find_package(nlohmann_json 3.12.0 REQUIRED)
target_link_libraries(MusicMonitor PRIVATE nlohmann_json::nlohmann_json)
