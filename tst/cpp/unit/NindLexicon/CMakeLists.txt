add_executable(NindLexiconUnitTests
  NindLexiconFileTest.cpp
  NindLexiconTest.cpp
)
target_link_libraries(NindLexiconUnitTests NindLexicon NindBasics nind_doctest_main)
add_test(NAME NindLexiconUnitTests COMMAND NindLexiconUnitTests)
set_tests_properties(NindLexiconUnitTests PROPERTIES TIMEOUT 600
  ENVIRONMENT "${NIND_TEST_ENVIRONMENT}")
