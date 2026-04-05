cmake_minimum_required(VERSION 3.16)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
project(Ext4Windows LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# WinFsp
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
find_package(WinFsp REQUIRED)

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
)

target_include_directories(ext4windows PRIVATE src)

target_link_libraries(ext4windows PRIVATE
    lwext4
    WinFsp::WinFsp
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
