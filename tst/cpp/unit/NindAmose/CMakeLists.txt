add_executable(NindAmoseUnitTests
  NindLexiconAmoseTest.cpp
  NindTermAmoseTest.cpp
  NindLocalAmoseTest.cpp
)
target_link_libraries(NindAmoseUnitTests NindAmose NindIndex NindRetrolexicon NindBasics nind_doctest_main)
add_test(NAME NindAmoseUnitTests COMMAND NindAmoseUnitTests)
set_tests_properties(NindAmoseUnitTests PROPERTIES TIMEOUT 600
  ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1")
