include_directories(${CMAKE_SOURCE_DIR}/src/cpp )

########### next target ###############

SET(NindIndex_LIB_SRCS
  NindIndex.h
  NindIndex.cpp
  NindLexiconIndex.h
  NindLexiconIndex.cpp
  NindTermIndex.h
  NindTermIndex.cpp
  NindLocalIndex.h
  NindLocalIndex.cpp
  NindIndexMerge.h
  NindIndexMerge.cpp
  NindTokenizer.h
  NindTokenizer.cpp
  NindDocNames.h
  NindDocNames.cpp
  NindIndexVerify.h
  NindIndexVerify.cpp
  NindIndexUpdate.h
  NindIndexUpdate.cpp
  NindBm25Scorer.h
  NindBm25Scorer.cpp
)
# scores reproduced bit for bit (nind's Python NindEngine): no fused multiply-adds
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  set_source_files_properties(NindBm25Scorer.cpp PROPERTIES COMPILE_OPTIONS -ffp-contract=off)
endif()

add_library(NindIndex SHARED ${NindIndex_LIB_SRCS})

target_link_libraries(NindIndex NindBasics NindRetrolexicon)

set_target_properties(NindIndex PROPERTIES VERSION ${NIND_LIB_VERSION} SOVERSION ${NIND_LIB_SOVERSION})
install(TARGETS NindIndex DESTINATION lib)

########### install files ###############
install(FILES NindIndex.h NindLexiconIndex.h NindTermIndex.h NindLocalIndex.h NindIndexMerge.h NindTokenizer.h NindDocNames.h NindIndexVerify.h NindIndexUpdate.h NindBm25Scorer.h DESTINATION include/NindIndex)
