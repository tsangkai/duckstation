execute_process(COMMAND git rev-parse --abbrev-ref HEAD
                OUTPUT_VARIABLE BRANCH)
string(STRIP "${BRANCH}" BRANCH)

execute_process(COMMAND git describe --tags --dirty --exclude latest
                OUTPUT_VARIABLE TAG)
string(STRIP "${TAG}" TAG)

set(VERSION_FILE "scmversion.cpp")

set(VERSION "const char* g_scm_branch_str = \"${BRANCH}\";
const char* g_scm_tag_str = \"${TAG}\";
")

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${VERSION_FILE})
  file(READ ${CMAKE_CURRENT_SOURCE_DIR}/${VERSION_FILE} EXISTING_VERSION)
else()
  set(EXISTING_VERSION "")
endif()

message("Current scmversion: ${TAG} (${BRANCH})")

if(NOT "${VERSION}" STREQUAL "${EXISTING_VERSION}")
  file(WRITE ${VERSION_FILE} "${VERSION}")
  message("Writing ${VERSION_FILE}...")
else()
  message("Skipping writing ${VERSION_FILE}, file is up to date")
endif()
