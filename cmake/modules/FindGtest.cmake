cmake_minimum_required(VERSION 2.6.4)

include(FindPackageHandleStandardArgs)


#------------------------
#
# Add includes and  libraries required for using Google Test.
#
# Output: GTEST_LIBRARIES and GTEST_INCLUDE_DIR
#------------------------

# Setup the paths and hints for Google Test
set(_GTEST_ROOT_PATHS "${PROJECT_SOURCE_DIR}/lib/googletest/")
set(_GTEST_ROOT_HINTS ${GTEST_ROOT_DIR} $ENV{GTEST_ROOT_DIR})
if(NOT WIN32)
  set(_GTEST_ROOT_PATHS "${_GTEST_ROOT_PATHS}" "/usr/" "/usr/local/")
endif()
set(_GTEST_ROOT_HINTS_AND_PATHS
  ${_GTEST_ROOT_HINTS}
  ${_GTEST_ROOT_PATHS})

# Ensure Google Test was found
find_path(GTEST_INCLUDE_DIR
  NAMES gtest/gtest.h
  HINTS ${_GTEST_ROOT_HINTS_AND_PATHS}
  PATH_SUFFIXES include)
find_library(GTEST_LIBRARY
  NAMES libgtest.so gtest libgtest
  HINTS ${_GTEST_ROOT_HINTS_AND_PATHS}
  PATH_SUFFIXES build/${CMAKE_CXX_COMPILER_ID}/${CMAKE_BUILD_TYPE} lib lib64)
find_library(GTEST_MAIN_LIBRARY
  NAMES libgtest_main.a gtest_main libgtest_main
  HINTS ${_GTEST_ROOT_HINTS_AND_PATHS}
  PATH_SUFFIXES build/${CMAKE_CXX_COMPILER_ID}/${CMAKE_BUILD_TYPE} lib lib64)
find_package_handle_standard_args(Gtest "Could NOT find Google Test, try to set the path to the googletest root folder in the system variable GTEST_ROOT_DIR"
  GTEST_LIBRARY
  GTEST_MAIN_LIBRARY
  GTEST_INCLUDE_DIR)
set(GTEST_LIBRARIES ${GTEST_LIBRARY} ${GTEST_MAIN_LIBRARY})

