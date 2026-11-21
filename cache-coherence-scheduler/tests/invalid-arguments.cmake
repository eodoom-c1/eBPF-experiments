function(expect_rejection expected_error)
    execute_process(
        COMMAND "${BENCHMARK}" ${ARGN}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error
    )
    if(NOT result STREQUAL "1" OR NOT output STREQUAL "" OR
       NOT error MATCHES "${expected_error}")
        message(FATAL_ERROR
            "Expected rejection for ${ARGN}; exit=${result}; stdout=${output}; stderr=${error}")
    endif()
endfunction()

expect_rejection("--cpus requires exactly" --cpus=0,0,1)
expect_rejection("--cpus requires exactly" --cpus=0,0,1,1,2)
expect_rejection("--cpus requires exactly" --cpus=999999999999999999999,0,1,1)
expect_rejection("--iterations must be an integer" --cpus=0,0,1,1 --iterations=0)
expect_rejection("--placement must be baseline or colocated"
    --cpus=0,0,1,1 --placement=invalid)
expect_rejection("--output requires a nonempty file path"
    --cpus=0,0,1,1 --output=)
expect_rejection("--rate must be an integer"
    --cpus=0,0,1,1 --rate=0)
expect_rejection("--rate must be an integer"
    --cpus=0,0,1,1 --rate=999999999999999999999)
expect_rejection("Requested rate and work exceed the supported timing range"
    --cpus=0,0,1,1 --iterations=4611686018427387903 --rate=1)
expect_rejection("doesn't allow an argument"
    --cpus=0,0,1,1 --pmu=enabled)
expect_rejection("--iterations must be an integer"
    --pmu --cpus=0,0,1,1 --iterations=0)

set(results_file "${CMAKE_CURRENT_BINARY_DIR}/static-placement-rejected-results.txt")
set(existing_results "previous results\n")
file(WRITE "${results_file}" "${existing_results}")
expect_rejection("--iterations must be an integer"
    --pmu --cpus=0,0,1,1 "--output=${results_file}" --iterations=0)
file(READ "${results_file}" retained_results)
file(REMOVE "${results_file}")
if(NOT retained_results STREQUAL existing_results)
    message(FATAL_ERROR "Rejected arguments changed an existing results file")
endif()
