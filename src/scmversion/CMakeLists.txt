# _scmversion.cpp is a dummy file that will never be created, so the command will always be run
add_custom_command(OUTPUT scmversion.cpp _scmversion.cpp
                   COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_SOURCE_DIR}/gen_scmversion.cmake)

add_library(scmversion
  scmversion.cpp
  scmversion.h
)

