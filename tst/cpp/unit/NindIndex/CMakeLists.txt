add_executable(NindIndexUnitTests
  NindLexiconIndexTest.cpp
  NindTermIndexTest.cpp
  NindLocalIndexTest.cpp
  NindIndexRobustnessTest.cpp
  NindIndexMergeTest.cpp
  NindBulkTest.cpp
  NindTokenizerTest.cpp
  NindDocNamesTest.cpp
  NindGoldenTest.cpp
)
target_compile_definitions(NindIndexUnitTests PRIVATE NIND_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/golden/v1")
target_link_libraries(NindIndexUnitTests NindIndex NindRetrolexicon NindBasics nind_doctest_main)
add_test(NAME NindIndexUnitTests COMMAND NindIndexUnitTests)
set_tests_properties(NindIndexUnitTests PROPERTIES TIMEOUT 600
  ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1")
