# Exercise transaction commands through the real parser, CLI, database, WAL,
# snapshot, and restart path. Runtime files live outside the repository.
if(DEFINED ENV{TEMP})
    set(temp_root "$ENV{TEMP}")
elseif(DEFINED ENV{TMPDIR})
    set(temp_root "$ENV{TMPDIR}")
else()
    set(temp_root "/tmp")
endif()
string(RANDOM LENGTH 24 ALPHABET 0123456789abcdef tag)
set(work "${temp_root}/minidb-transaction-${tag}")
set(db "${work}/database.snapshot")
file(MAKE_DIRECTORY "${work}")

function(fail message_text)
    file(REMOVE_RECURSE "${work}")
    message(FATAL_ERROR "${message_text}")
endfunction()

function(run_cli name input)
    file(WRITE "${work}/${name}.input" "${input}")
    execute_process(COMMAND "${CLI}" "${db}" INPUT_FILE "${work}/${name}.input"
        OUTPUT_VARIABLE output ERROR_VARIABLE errors RESULT_VARIABLE result TIMEOUT 15)
    if(NOT "${result}" STREQUAL "0")
        fail("${name} failed (${result}): ${errors}")
    endif()
    set("${name}_output" "${output}" PARENT_SCOPE)
endfunction()

function(require_output output expected label)
    string(FIND "${output}" "${expected}" found)
    if(found EQUAL -1)
        fail("${label}: missing output '${expected}'\nActual output:\n${output}")
    endif()
endfunction()

run_cli(commit "BEGIN\nSET name Aarya\nSET language C++\nGET name\nCOMMIT\nEXIT\n")
require_output("${commit_output}" "MiniDB> Aarya" "transaction-local GET")

run_cli(restart "GET name\nGET language\nEXIT\n")
require_output("${restart_output}" "MiniDB> Aarya" "committed name after restart")
require_output("${restart_output}" "MiniDB> C++" "committed language after restart")

run_cli(rollback "BEGIN\nSET name Temporary\nGET name\nROLLBACK\nGET name\nEXIT\n")
require_output("${rollback_output}" "MiniDB> Temporary" "transaction-local overwrite")
require_output("${rollback_output}" "MiniDB> Aarya" "rollback restored old value")

run_cli(invalid "COMMIT\nROLLBACK\nBEGIN\nBEGIN\nROLLBACK\nEXIT\n")
require_output("${invalid_output}" "INVALID_TRANSACTION_STATE" "invalid transaction state")

run_cli(exit_rollback "BEGIN\nSET discarded value\nEXIT\n")
require_output("${exit_rollback_output}" "Rolled back active transaction." "EXIT rollback")
run_cli(exit_verify "EXISTS discarded\nEXIT\n")
require_output("${exit_verify_output}" "MiniDB> false" "EXIT rollback persistence")

file(REMOVE_RECURSE "${work}")
