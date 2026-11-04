cmake_minimum_required(VERSION 3.10)

include_directories(${CMAKE_SOURCE_DIR}/src/cpp)
include_directories(${CMAKE_SOURCE_DIR}/tst/cpp/unit)

# single translation unit holding doctest's implementation and main()
add_library(nind_doctest_main STATIC doctest_main.cpp)

# sanitizer options of every unit test: a test fails on a leak, and on an allocation over 2 GB
# (a size read from a corrupt file, say) instead of growing until the machine runs out of memory
set(NIND_TEST_ENVIRONMENT
  "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1"
  "ASAN_OPTIONS=detect_leaks=1:max_allocation_size_mb=2048"
  "TSAN_OPTIONS=max_allocation_size_mb=2048")

add_subdirectory(NindBasics)
add_subdirectory(NindLexicon)
add_subdirectory(NindRetrolexicon)
add_subdirectory(NindIndex)
add_subdirectory(NindAmose)

# every installed header compiles on its own against the installed headers only
add_test(NAME InstalledHeaders
  COMMAND bash ${CMAKE_SOURCE_DIR}/tst/cpp/install/check-installed-headers.sh ${CMAKE_BINARY_DIR} ${CMAKE_CXX_COMPILER})
set_tests_properties(InstalledHeaders PROPERTIES TIMEOUT 300)
