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
