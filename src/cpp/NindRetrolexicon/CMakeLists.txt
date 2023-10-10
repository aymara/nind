include_directories(${CMAKE_SOURCE_DIR}/src/cpp )

########### next target ###############

SET(NindRetrolexicon_LIB_SRCS
  NindRetrolexicon.h
  NindRetrolexicon.cpp
)

add_library(NindRetrolexicon SHARED ${NindRetrolexicon_LIB_SRCS})

target_link_libraries(NindRetrolexicon NindBasics)

set_target_properties(NindRetrolexicon PROPERTIES VERSION ${NIND_LIB_VERSION} SOVERSION ${NIND_LIB_SOVERSION})
install(TARGETS NindRetrolexicon DESTINATION lib)

########### install files ###############
install(FILES NindRetrolexicon.h DESTINATION include/NindRetrolexicon)
