add_executable(NindBasicsUnitTests
  NindFileTest.cpp
  NindPadFileTest.cpp
  NindBasicsRobustnessTest.cpp
)
target_link_libraries(NindBasicsUnitTests NindBasics nind_doctest_main)
add_test(NAME NindBasicsUnitTests COMMAND NindBasicsUnitTests)
set_tests_properties(NindBasicsUnitTests PROPERTIES TIMEOUT 600
  ENVIRONMENT "${NIND_TEST_ENVIRONMENT}")
