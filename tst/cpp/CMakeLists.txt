include_directories(${CMAKE_SOURCE_DIR}/src/cpp/)
include_directories(${CMAKE_SOURCE_DIR}/tst/cpp/)
#add_subdirectory(NindLexicon)
add_subdirectory(NindIndex)
#add_subdirectory(NindTests)
add_subdirectory(NindSearch)
add_subdirectory(NindAmose)
#add_subdirectory(NindBasics)

########### GoogleTest-based unit tests, run through ctest ###############
find_package(GTest QUIET)
if (GTest_FOUND OR GTEST_FOUND)
  message("GTest found: unit tests enabled (tst/cpp/unit)")
  add_subdirectory(unit)
else()
  message(WARNING "GTest not found: unit tests (tst/cpp/unit) will not be built. "
                   "Install libgtest-dev (Debian/Ubuntu) to enable them.")
endif()
