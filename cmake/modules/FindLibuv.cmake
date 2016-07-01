cmake_minimum_required(VERSION 2.6.4)

include(FindPackageHandleStandardArgs)


#------------------------
#
# Add includes and  libraries required for using libuv.
#
# Output: LIBUV_LIBRARY and LIBUV_INCLUDE_DIR
#------------------------

# Setup the paths and hints for libuv
set(_LIBUV_ROOT_PATHS "${PROJECT_SOURCE_DIR}/lib/libuv/")
set(_LIBUV_ROOT_HINTS ${LIBUV_ROOT_DIR} $ENV{LIBUV_ROOT_DIR})
if(NOT WIN32)
  set(_LIBUV_ROOT_PATHS "${_LIBUV_ROOT_PATHS}" "/usr/" "/usr/local/")
endif()
set(_LIBUV_ROOT_HINTS_AND_PATHS
  ${_LIBUV_ROOT_HINTS}
  ${_LIBUV_ROOT_PATHS})

# Ensure libuv was found
find_path(LIBUV_INCLUDE_DIR
  NAMES uv.h
  HINTS ${_LIBUV_ROOT_HINTS_AND_PATHS}
  PATH_SUFFIXES include)
find_library(LIBUV_LIBRARY
  NAMES uv libuv
  HINTS ${_LIBUV_ROOT_HINTS_AND_PATHS}
  PATH_SUFFIXES lib lib64)
find_package_handle_standard_args(Libuv "Could NOT find libuv, try to set the path to the libuv root folder in the system variable LIBUV_ROOT_DIR"
  LIBUV_LIBRARY
  LIBUV_INCLUDE_DIR)

