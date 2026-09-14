add_executable(NindAmoseUnitTests
  NindLexiconAmoseTest.cpp
  NindTermAmoseTest.cpp
  NindLocalAmoseTest.cpp
)
target_link_libraries(NindAmoseUnitTests NindAmose NindIndex NindRetrolexicon NindBasics GTest::gtest GTest::gtest_main)
gtest_discover_tests(NindAmoseUnitTests)
