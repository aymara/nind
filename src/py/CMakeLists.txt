add_subdirectory(amose)
# ########### install files ###############
file(GLOB pys *.py)
install(PROGRAMS
  ${pys}
  DESTINATION bin)

