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
  NindConcurrentReadTest.cpp
  NindFormatV2Test.cpp
)
target_compile_definitions(NindIndexUnitTests PRIVATE NIND_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/golden")
find_package(Threads REQUIRED)
target_link_libraries(NindIndexUnitTests NindIndex NindRetrolexicon NindBasics nind_doctest_main Threads::Threads)
add_test(NAME NindIndexUnitTests COMMAND NindIndexUnitTests)
set_tests_properties(NindIndexUnitTests PROPERTIES TIMEOUT 600
  ENVIRONMENT "${NIND_TEST_ENVIRONMENT}")
