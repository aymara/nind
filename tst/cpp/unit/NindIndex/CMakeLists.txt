add_executable(NindIndexUnitTests
  NindLexiconIndexTest.cpp
  NindTermIndexTest.cpp
  NindLocalIndexTest.cpp
)
target_link_libraries(NindIndexUnitTests NindIndex NindRetrolexicon NindBasics GTest::gtest GTest::gtest_main)
gtest_discover_tests(NindIndexUnitTests)
