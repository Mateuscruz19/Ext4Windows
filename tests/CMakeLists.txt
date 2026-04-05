# Test suite using Catch2 (amalgamated, no install needed)

# Catch2 amalgamated source
add_library(catch2_main STATIC
    ${CMAKE_SOURCE_DIR}/external/catch2/catch_amalgamated.cpp
)
target_include_directories(catch2_main PUBLIC
    ${CMAKE_SOURCE_DIR}/external/catch2
)

# Common include dirs for test files
set(TEST_INCLUDES
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/external/lwext4/include
    ${CMAKE_BINARY_DIR}/external/lwext4/include
)

# --- Unit tests (no ext4/WinFsp dependencies) ---

add_executable(test_validation test_validation.cpp)
target_include_directories(test_validation PRIVATE ${TEST_INCLUDES})
target_link_libraries(test_validation PRIVATE catch2_main)
target_compile_definitions(test_validation PRIVATE UNICODE _UNICODE NOMINMAX)

add_executable(test_string_utils test_string_utils.cpp)
target_include_directories(test_string_utils PRIVATE ${TEST_INCLUDES})
target_link_libraries(test_string_utils PRIVATE catch2_main)
target_compile_definitions(test_string_utils PRIVATE UNICODE _UNICODE NOMINMAX)

add_executable(test_timestamps test_timestamps.cpp)
target_include_directories(test_timestamps PRIVATE ${TEST_INCLUDES})
target_link_libraries(test_timestamps PRIVATE catch2_main)
target_compile_definitions(test_timestamps PRIVATE UNICODE _UNICODE NOMINMAX)

add_executable(test_permissions test_permissions.cpp)
target_include_directories(test_permissions PRIVATE ${TEST_INCLUDES})
target_link_libraries(test_permissions PRIVATE catch2_main)
target_compile_definitions(test_permissions PRIVATE UNICODE _UNICODE NOMINMAX)

add_executable(test_format_size test_format_size.cpp)
target_include_directories(test_format_size PRIVATE ${TEST_INCLUDES})
target_link_libraries(test_format_size PRIVATE catch2_main)
target_compile_definitions(test_format_size PRIVATE UNICODE _UNICODE NOMINMAX)

# --- Integration tests (need lwext4 + block device code) ---

add_executable(test_blockdev
    test_blockdev.cpp
    ${CMAKE_SOURCE_DIR}/src/blockdev_file.cpp
    ${CMAKE_SOURCE_DIR}/src/blockdev_partition.cpp
)
target_include_directories(test_blockdev PRIVATE ${TEST_INCLUDES})
target_link_libraries(test_blockdev PRIVATE catch2_main lwext4)
target_compile_definitions(test_blockdev PRIVATE UNICODE _UNICODE NOMINMAX)

# Register all tests with CTest
enable_testing()
add_test(NAME validation    COMMAND test_validation)
add_test(NAME string_utils  COMMAND test_string_utils)
add_test(NAME timestamps    COMMAND test_timestamps)
add_test(NAME permissions   COMMAND test_permissions)
add_test(NAME format_size   COMMAND test_format_size)
add_test(NAME blockdev      COMMAND test_blockdev)
