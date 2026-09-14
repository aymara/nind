cmake_minimum_required(VERSION 3.10)

include(GoogleTest)
include_directories(${CMAKE_SOURCE_DIR}/src/cpp)
include_directories(${CMAKE_SOURCE_DIR}/tst/cpp/unit)

add_subdirectory(NindBasics)
add_subdirectory(NindLexicon)
add_subdirectory(NindRetrolexicon)
add_subdirectory(NindIndex)
add_subdirectory(NindAmose)
