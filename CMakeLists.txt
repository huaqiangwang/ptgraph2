cmake_minimum_required(VERSION 3.10)

project(ptgraph LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

set(SOURCES
    src/dlfilter.cpp
    src/graph.cpp
    src/perfetto.cpp
)

add_library(ptgraph SHARED ${SOURCES})

set_target_properties(ptgraph PROPERTIES
    OUTPUT_NAME "ptgraph"
    PREFIX "lib"
)

target_include_directories(ptgraph PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

find_package(Threads REQUIRED)
find_package(nlohmann_json 3.2.0 REQUIRED)

target_link_libraries(ptgraph PRIVATE
    Threads::Threads
    rt
    nlohmann_json::nlohmann_json
)

target_compile_options(ptgraph PRIVATE
    -Wall
    -Wextra
)

# mperf parallel multi-process launcher
add_executable(mperf src/mperf.cpp src/graph.cpp src/perfetto.cpp)
target_include_directories(mperf PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(mperf PRIVATE
    Threads::Threads
    rt
    nlohmann_json::nlohmann_json
)
target_compile_options(mperf PRIVATE
    -Wall
    -Wextra
)

add_executable(test_shm test/test_shm.cpp src/graph.cpp src/perfetto.cpp)
target_include_directories(test_shm PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(test_shm PRIVATE
    Threads::Threads
    rt
    nlohmann_json::nlohmann_json
)
target_compile_options(test_shm PRIVATE
    -Wall
    -Wextra
)

add_executable(test_json test/test_json.cpp src/graph.cpp src/perfetto.cpp)
target_include_directories(test_json PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(test_json PRIVATE
    Threads::Threads
    rt
    nlohmann_json::nlohmann_json
)
target_compile_options(test_json PRIVATE
    -Wall
    -Wextra
)
