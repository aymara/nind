# Standalone CMake project building the `nind._native` pybind11 extension
# module. Kept separate from the top-level CMakeLists.txt (which builds the
# full C++ library/test suite) so that `pip`/`uv build` can point
# scikit-build-core straight at this directory without pulling in tests,
# NindAmose, or CPack packaging.
cmake_minimum_required(VERSION 3.15)
project(nind_native LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Python REQUIRED COMPONENTS Interpreter Development.Module)

execute_process(
  COMMAND "${Python_EXECUTABLE}" -m pybind11 --cmakedir
  OUTPUT_VARIABLE pybind11_DIR
  OUTPUT_STRIP_TRAILING_WHITESPACE)
find_package(pybind11 CONFIG REQUIRED)

# The core index stack lives one level up, in src/cpp/. Its sources are
# compiled directly into this single extension module (rather than linked
# against the separate NindBasics/NindRetrolexicon/NindLexicon/NindIndex
# shared libraries built by the top-level project) so the wheel ships one
# self-contained .so with no internal RPATH/dlopen concerns.
set(NIND_CPP_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)

pybind11_add_module(_native
  bindings.cpp
  ${NIND_CPP_ROOT}/NindBasics/NindFile.cpp
  ${NIND_CPP_ROOT}/NindBasics/NindPadFile.cpp
  ${NIND_CPP_ROOT}/NindBasics/NindSignalCatcher.cpp
  ${NIND_CPP_ROOT}/NindRetrolexicon/NindRetrolexicon.cpp
  ${NIND_CPP_ROOT}/NindLexicon/NindLexicon.cpp
  ${NIND_CPP_ROOT}/NindLexicon/NindLexiconFile.cpp
  ${NIND_CPP_ROOT}/NindIndex/NindIndex.cpp
  ${NIND_CPP_ROOT}/NindIndex/NindLexiconIndex.cpp
  ${NIND_CPP_ROOT}/NindIndex/NindTermIndex.cpp
  ${NIND_CPP_ROOT}/NindIndex/NindLocalIndex.cpp
)

target_include_directories(_native PRIVATE ${NIND_CPP_ROOT})

if(MSVC)
  # NindCommonExport.h's DLLExportLexicon macro expands to dllimport unless
  # this is defined, which fails to link since these classes are defined
  # (not just declared) in this same module.
  target_compile_definitions(_native PRIVATE NindLexicon_EXPORTS)
endif()

install(TARGETS _native DESTINATION nind)
