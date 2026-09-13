# Each test file becomes its own executable and its own CTest test.
#
# One binary per source keeps the tests independent: a crash or an infinite
# loop in one file cannot take the rest of the suite down with it, and CTest
# can run them in parallel.

# The test framework header lives beside the tests, not in include/, because it
# is not part of the MiniDB public interface.
add_library(minidb_test_framework INTERFACE)
target_include_directories(minidb_test_framework INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

function(minidb_add_test test_name)
    add_executable(${test_name} ${test_name}.cpp)
    target_link_libraries(${test_name} PRIVATE minidb_core minidb_test_framework minidb_warnings)
    add_test(NAME ${test_name} COMMAND ${test_name})
    # A test that hangs should fail the build rather than stall CI forever.
    set_tests_properties(${test_name} PROPERTIES TIMEOUT 60)
endfunction()

minidb_add_test(test_command_parser)
minidb_add_test(test_database)
minidb_add_test(test_hash_table)
minidb_add_test(test_result)
minidb_add_test(test_storage)
minidb_add_test(test_wal)
minidb_add_test(test_recovery)

add_executable(recovery_process recovery_process.cpp)
target_link_libraries(recovery_process PRIVATE minidb_core minidb_warnings)
add_test(NAME test_recovery_process COMMAND ${CMAKE_COMMAND}
    "-DHELPER=$<TARGET_FILE:recovery_process>" "-DCLI=$<TARGET_FILE:minidb>"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/recovery_process.cmake")
set_tests_properties(test_recovery_process PROPERTIES TIMEOUT 60)
minidb_add_test(test_lru_cache)
