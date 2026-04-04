# FindWinFsp.cmake
# Finds the WinFsp SDK installation on Windows.
#
# Provides:
#   WinFsp::WinFsp  - Imported interface target
#   WinFsp_FOUND    - TRUE if found
#   WINFSP_INCLUDE_DIR - Path to inc/
#   WINFSP_LIBRARY     - Path to winfsp-x64.lib

# Try registry first
get_filename_component(WINFSP_INSTALL_DIR
    "[HKEY_LOCAL_MACHINE\\SOFTWARE\\WinFsp;InstallDir]"
    ABSOLUTE CACHE)

# Fallback paths
set(_winfsp_search_paths
    "${WINFSP_INSTALL_DIR}"
    "C:/Program Files (x86)/WinFsp"
    "C:/Program Files/WinFsp"
)

find_path(WINFSP_INCLUDE_DIR
    NAMES winfsp/winfsp.h
    PATHS ${_winfsp_search_paths}
    PATH_SUFFIXES inc
)

find_library(WINFSP_LIBRARY
    NAMES winfsp-x64
    PATHS ${_winfsp_search_paths}
    PATH_SUFFIXES lib
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(WinFsp
    REQUIRED_VARS WINFSP_INCLUDE_DIR WINFSP_LIBRARY
)

if(WinFsp_FOUND AND NOT TARGET WinFsp::WinFsp)
    add_library(WinFsp::WinFsp INTERFACE IMPORTED)
    set_target_properties(WinFsp::WinFsp PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${WINFSP_INCLUDE_DIR}"
        INTERFACE_LINK_LIBRARIES "${WINFSP_LIBRARY}"
    )
endif()

mark_as_advanced(WINFSP_INCLUDE_DIR WINFSP_LIBRARY)
