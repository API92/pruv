# pruv-config
# --------------
#
# This module defines
#
# ::
#
#   pruv_FOUND - Set to true when pruv is found.
#   pruv_INCLUDE_DIR - the directory of the pruv headers
#   pruv_LIBRARY - the pruv library needed for linking

find_path(pruv_INCLUDE_DIR
    NAME pruv/dispatcher.hpp
    PATHS ${CMAKE_CURRENT_LIST_DIR}/include)

find_library(pruv_LIBRARY
    NAMES pruv libpruv
    PATHS ${CMAKE_CURRENT_LIST_DIR}
    PATH_SUFFIXES build/${CMAKE_CXX_COMPILER_ID}/${CMAKE_BUILD_TYPE})

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(pruv
    "Could NOT find pruv, try to set the path to the pruv root folder in the variable pruv_DIR"
    pruv_LIBRARY
    pruv_INCLUDE_DIR)
