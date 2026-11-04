add_executable(NindAmoseUnitTests
  NindLexiconAmoseTest.cpp
  NindTermAmoseTest.cpp
  NindLocalAmoseTest.cpp
  NindAmoseMergeTest.cpp
)
target_link_libraries(NindAmoseUnitTests NindAmose NindIndex NindRetrolexicon NindBasics nind_doctest_main)
add_test(NAME NindAmoseUnitTests COMMAND NindAmoseUnitTests)
set_tests_properties(NindAmoseUnitTests PROPERTIES TIMEOUT 600
  ENVIRONMENT "${NIND_TEST_ENVIRONMENT}")
