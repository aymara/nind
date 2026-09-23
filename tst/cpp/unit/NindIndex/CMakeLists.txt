add_executable(NindIndexUnitTests
  NindLexiconIndexTest.cpp
  NindTermIndexTest.cpp
  NindLocalIndexTest.cpp
  NindIndexRobustnessTest.cpp
)
target_link_libraries(NindIndexUnitTests NindIndex NindRetrolexicon NindBasics nind_doctest_main)
add_test(NAME NindIndexUnitTests COMMAND NindIndexUnitTests)
set_tests_properties(NindIndexUnitTests PROPERTIES TIMEOUT 600
  ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1")
