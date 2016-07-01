cmake_minimum_required(VERSION 2.6.4)

include(FindPackageHandleStandardArgs)


#------------------------
#
# Add includes and  libraries required for using http-parser.
#
# Output: HTTP_PARSER_LIBRARY and HTTP_PARSER_INCLUDE_DIR
#------------------------

# Setup the paths and hints for http-parser
set(_HTTP_PARSER_ROOT_PATHS "${PROJECT_SOURCE_DIR}/lib/http-parser/")
set(_HTTP_PARSER_ROOT_HINTS ${HTTP_PARSER_ROOT_DIR} $ENV{HTTP_PARSER_ROOT_DIR})
if(NOT WIN32)
    set(_HTTP_PARSER_ROOT_PATHS "${_HTTP_PARSER_ROOT_PATHS}" "/usr/" "/usr/local/")
endif()
set(_HTTP_PARSER_ROOT_HINTS_AND_PATHS
  ${_HTTP_PARSER_ROOT_HINTS}
  ${_HTTP_PARSER_ROOT_PATHS})

# Ensure http-parser was found
find_path(HTTP_PARSER_INCLUDE_DIR
  NAMES http_parser.h
  HINTS ${_HTTP_PARSER_ROOT_HINTS_AND_PATHS}
  PATH_SUFFIXES include)
find_library(HTTP_PARSER_LIBRARY
  NAMES http_parser libhttp_parser
  HINTS ${_HTTP_PARSER_ROOT_HINTS_AND_PATHS}
  PATH_SUFFIXES lib lib64)
find_package_handle_standard_args(HttpParser "Could NOT find http-parser, try to set the path to the http-parser root folder in the system variable HTTP_PARSER_ROOT_DIR"
  HTTP_PARSER_LIBRARY
  HTTP_PARSER_INCLUDE_DIR)

