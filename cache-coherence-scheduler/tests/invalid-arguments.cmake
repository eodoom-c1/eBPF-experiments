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
expect_rejection("--rates requires exactly four"
    --cpus=0,0,1,1 --rates=1,2,3)
expect_rejection("--rates requires exactly four"
    --cpus=0,0,1,1 --rates=1,2,3,4,5)
expect_rejection("--rates requires exactly four"
    --cpus=0,0,1,1 --rates=1,,3,4)
expect_rejection("--rates requires exactly four"
    --cpus=0,0,1,1 --rates=1,2,3,)
expect_rejection("--rates requires exactly four"
    --cpus=0,0,1,1 --rates=1,-2,3,4)
expect_rejection("--rates requires exactly four"
    --cpus=0,0,1,1 --rates=1,2,3,18446744073709551616)
expect_rejection("--computes requires exactly four"
    --cpus=0,0,1,1 --computes=)
expect_rejection("--computes requires exactly four"
    --cpus=0,0,1,1 --computes=0,1,2,1000001)
expect_rejection("Use either --rate or --rates, not both"
    --cpus=0,0,1,1 --rate=1 --rates=1,2,3,4)
expect_rejection("Use either --rate or --rates, not both"
    --cpus=0,0,1,1 --rates=1,2,3,4 --rate=1)
expect_rejection("Use either --compute or --computes, not both"
    --cpus=0,0,1,1 --compute=1 --computes=0,1,2,3)
expect_rejection("Use either --compute or --computes, not both"
    --cpus=0,0,1,1 --computes=0,1,2,3 --compute=1)
expect_rejection("Requested rate and work exceed the supported timing range for thread 4"
    --cpus=0,0,1,1 --iterations=4611686018427387903 --rates=0,0,0,1)
expect_rejection("doesn't allow an argument"
    --cpus=0,0,1,1 --pmu=enabled)
expect_rejection("--iterations must be an integer"
    --pmu --cpus=0,0,1,1 --iterations=0)
expect_rejection("--pmu-profile must be generic or intel-n150"
    --cpus=0,0,1,1 --pmu-profile=invalid)
expect_rejection("--pmu-profile must be generic or intel-n150"
    --cpus=0,0,1,1 --pmu-profile=)
expect_rejection("--iterations must be an integer"
    --pmu-profile=intel-n150 --cpus=0,0,1,1 --iterations=0)

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

if(DEFINED SERVICE)
    set(BENCHMARK "${SERVICE}")
    expect_rejection("--cpus requires exactly" --cpus=0,0,1)
    expect_rejection("--placement must be baseline or colocated"
        --cpus=0,0,1,1 --placement=invalid)
    expect_rejection("--layout must be shared or padded"
        --cpus=0,0,1,1 --layout=invalid)
    expect_rejection("--base-port must be an integer"
        --cpus=0,0,1,1 --base-port=65533)
endif()
