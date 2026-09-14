add_executable(NindLexiconUnitTests
  NindLexiconFileTest.cpp
  NindLexiconTest.cpp
)
target_link_libraries(NindLexiconUnitTests NindLexicon NindBasics GTest::gtest GTest::gtest_main)
gtest_discover_tests(NindLexiconUnitTests)
