include_directories(${CMAKE_SOURCE_DIR}/src/cpp/)
include_directories(${CMAKE_SOURCE_DIR}/tst/cpp/)
#add_subdirectory(NindLexicon)
add_subdirectory(NindIndex)
#add_subdirectory(NindTests)
add_subdirectory(NindSearch)
add_subdirectory(NindAmose)
#add_subdirectory(NindBasics)

########### doctest-based unit tests, run through ctest ###############
# doctest is vendored (tst/cpp/unit/doctest.h): no external dependency, always built.
add_subdirectory(unit)
