add_executable(NindBasicsUnitTests
  NindFileTest.cpp
  NindPadFileTest.cpp
)
target_link_libraries(NindBasicsUnitTests NindBasics GTest::gtest GTest::gtest_main)
gtest_discover_tests(NindBasicsUnitTests)
