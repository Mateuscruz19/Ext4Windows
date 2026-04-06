# FindWebView2.cmake
# Finds the WebView2 SDK bundled in external/webview2/.
#
# The WebView2 SDK is the Microsoft library that lets you embed a
# Chromium-based web browser inside a native Windows application.
# Think of it like an <iframe> but for desktop apps — you get a
# real browser engine that can render HTML/CSS/JS.
#
# Docs: https://learn.microsoft.com/en-us/microsoft-edge/webview2/
#
# Provides:
#   WebView2::WebView2  - Imported interface target
#   WebView2_FOUND      - TRUE if found
#   WEBVIEW2_INCLUDE_DIR - Path to include/
#   WEBVIEW2_LIBRARY     - Path to WebView2LoaderStatic.lib

# The SDK lives in external/webview2 (extracted from NuGet package)
set(_webview2_root "${CMAKE_CURRENT_SOURCE_DIR}/external/webview2")

find_path(WEBVIEW2_INCLUDE_DIR
    NAMES WebView2.h
    PATHS "${_webview2_root}/build/native/include"
    NO_DEFAULT_PATH
)

find_library(WEBVIEW2_LIBRARY
    NAMES WebView2LoaderStatic
    PATHS "${_webview2_root}/build/native/x64"
    NO_DEFAULT_PATH
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(WebView2
    REQUIRED_VARS WEBVIEW2_INCLUDE_DIR WEBVIEW2_LIBRARY
)

if(WebView2_FOUND AND NOT TARGET WebView2::WebView2)
    add_library(WebView2::WebView2 INTERFACE IMPORTED)
    set_target_properties(WebView2::WebView2 PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${WEBVIEW2_INCLUDE_DIR}"
        INTERFACE_LINK_LIBRARIES "${WEBVIEW2_LIBRARY}"
    )
endif()

mark_as_advanced(WEBVIEW2_INCLUDE_DIR WEBVIEW2_LIBRARY)
