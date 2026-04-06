cmake_minimum_required(VERSION 3.16)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
project(Ext4Windows VERSION 1.1.0 LANGUAGES C CXX)

# Pass version to C++ code as a compile-time define
add_compile_definitions(EXT4W_VERSION="${CMAKE_PROJECT_VERSION}")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# WinFsp
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
find_package(WinFsp REQUIRED)

# WebView2 — embedded Chromium browser for the GUI
# The SDK lives in external/webview2/ (downloaded from NuGet).
# Uses WebView2LoaderStatic.lib so no extra DLL is needed at runtime.
# The WebView2 Runtime itself is built into Windows 10/11 (via Edge).
find_package(WebView2 REQUIRED)

# lwext4
set(LIB_ONLY ON CACHE BOOL "" FORCE)
add_subdirectory(external/lwext4)
target_include_directories(lwext4 PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/external/lwext4/include"
    "${CMAKE_CURRENT_BINARY_DIR}/external/lwext4/include"
)

add_executable(ext4windows
    src/main.cpp
    src/ext4_filesystem.cpp
    src/blockdev_file.cpp
    src/blockdev_partition.cpp
    src/partition_scanner.cpp
    src/server.cpp
    src/client.cpp
    src/tray_icon.cpp
    src/interactive.cpp
    src/gui/gui_main.cpp
    src/ext4windows.rc
)

target_include_directories(ext4windows PRIVATE src)

target_link_libraries(ext4windows PRIVATE
    lwext4
    WinFsp::WinFsp
    WebView2::WebView2
)

target_compile_definitions(ext4windows PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
)

# Copy winfsp-x64.dll next to the executable so it can find it at runtime
get_filename_component(_winfsp_sdk_dir "${WINFSP_LIBRARY}" DIRECTORY)
get_filename_component(_winfsp_root "${_winfsp_sdk_dir}" DIRECTORY)
set(_winfsp_dll "${_winfsp_root}/bin/winfsp-x64.dll")
if(EXISTS "${_winfsp_dll}")
    add_custom_command(TARGET ext4windows POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_winfsp_dll}"
            "$<TARGET_FILE_DIR:ext4windows>"
        COMMENT "Copying winfsp-x64.dll to build directory"
    )
endif()

# Tests (Catch2)
add_subdirectory(tests)
