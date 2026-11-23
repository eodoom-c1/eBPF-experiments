#define main static_placement_program_main
#include "../static-placement.c"
#undef main

static void check_options(
    int argument_count,
    char **arguments,
    const uint64_t expected_rates[WORKER_COUNT],
    const uint64_t expected_compute[WORKER_COUNT],
    const char *expected_json)
{
    /* GNU getopt must be reinitialized between independent invocations. */
    optind = 0;
    struct benchmark_options options = parse_benchmark_options(argument_count, arguments);
    for (int worker_index = 0; worker_index < WORKER_COUNT; ++worker_index)
    {
        if (options.target_increments_per_second[worker_index] != expected_rates[worker_index] ||
            options.compute_steps_per_increment[worker_index] != expected_compute[worker_index])
        {
            exit_with_error("Unexpected workload settings for thread %d", worker_index + 1);
        }
    }

    FILE *output = tmpfile();
    if (output == NULL)
    {
        exit_with_error("tmpfile: %s", strerror(errno));
    }
    write_workload_settings(output, &options);
    if (ferror(output) || fflush(output) != 0 || fseek(output, 0, SEEK_SET) != 0)
    {
        exit_with_error("Cannot read back workload settings");
    }
    char actual_json[512] = {0};
    size_t bytes_read = fread(actual_json, 1, sizeof(actual_json) - 1, output);
    if (ferror(output) || bytes_read == sizeof(actual_json) - 1 || fclose(output) != 0)
    {
        exit_with_error("Cannot read complete workload settings");
    }
    if (strcmp(actual_json, expected_json) != 0)
    {
        exit_with_error("Unexpected workload JSON:\n%s\nExpected:\n%s", actual_json, expected_json);
    }
}

int main(void)
{
    const uint64_t zero[WORKER_COUNT] = {0, 0, 0, 0};
    const uint64_t uniform_rates[WORKER_COUNT] = {2000, 2000, 2000, 2000};
    const uint64_t uniform_compute[WORKER_COUNT] = {2, 2, 2, 2};
    const uint64_t uneven_rates[WORKER_COUNT] = {0, 1000, 2000, 4000};
    const uint64_t uneven_compute[WORKER_COUNT] = {0, 1, 2, 3};

    char *defaults[] = {"static-placement", "--cpus=0,0,1,1", "--iterations=1000", NULL};
    check_options((int)(sizeof(defaults) / sizeof(defaults[0]) - 1), defaults, zero, zero,
        "\"iterations_per_thread\":1000,\"compute\":0,"
        "\"target_increments_per_second_per_thread\":0,\"nominal_work_seconds\":0.000000000,");

    char *scalar[] = {"static-placement", "--cpus=0,0,1,1", "--iterations=1000",
        "--rate=2000", "--compute=2", NULL};
    const char *uniform_json = "\"iterations_per_thread\":1000,\"compute\":2,"
        "\"target_increments_per_second_per_thread\":2000,\"nominal_work_seconds\":0.500000000,";
    check_options((int)(sizeof(scalar) / sizeof(scalar[0]) - 1), scalar, uniform_rates, uniform_compute,
        uniform_json);

    char *uniform_lists[] = {"static-placement", "--cpus=0,0,1,1", "--iterations=1000",
        "--rates=2000,2000,2000,2000", "--computes=2,2,2,2", NULL};
    check_options((int)(sizeof(uniform_lists) / sizeof(uniform_lists[0]) - 1), uniform_lists,
        uniform_rates, uniform_compute, uniform_json);

    char *uneven_lists[] = {"static-placement", "--cpus=0,0,1,1", "--iterations=1000",
        "--rates=0,1000,2000,4000", "--computes=0,1,2,3", "--placement=colocated", NULL};
    const char *uneven_json = "\"iterations_per_thread\":1000,\"compute\":null,"
        "\"target_increments_per_second_per_thread\":null,\"nominal_work_seconds\":null,";
    check_options((int)(sizeof(uneven_lists) / sizeof(uneven_lists[0]) - 1), uneven_lists,
        uneven_rates, uneven_compute, uneven_json);

    char *uneven_rate_only[] = {"static-placement", "--cpus=0,0,1,1", "--iterations=1000",
        "--rates=0,1000,2000,4000", "--compute=2", NULL};
    check_options((int)(sizeof(uneven_rate_only) / sizeof(uneven_rate_only[0]) - 1), uneven_rate_only,
        uneven_rates, uniform_compute, "\"iterations_per_thread\":1000,\"compute\":2,"
        "\"target_increments_per_second_per_thread\":null,\"nominal_work_seconds\":null,");

    char *uneven_compute_only[] = {"static-placement", "--cpus=0,0,1,1", "--iterations=1000",
        "--rate=2000", "--computes=0,1,2,3", NULL};
    check_options((int)(sizeof(uneven_compute_only) / sizeof(uneven_compute_only[0]) - 1),
        uneven_compute_only, uniform_rates, uneven_compute,
        "\"iterations_per_thread\":1000,\"compute\":null,"
        "\"target_increments_per_second_per_thread\":2000,\"nominal_work_seconds\":0.500000000,");

    char *unpaced_list[] = {"static-placement", "--cpus=0,0,1,1", "--iterations=1000",
        "--rates=0,0,0,0", "--computes=2,2,2,2", NULL};
    check_options((int)(sizeof(unpaced_list) / sizeof(unpaced_list[0]) - 1), unpaced_list,
        zero, uniform_compute, "\"iterations_per_thread\":1000,\"compute\":2,"
        "\"target_increments_per_second_per_thread\":0,\"nominal_work_seconds\":0.000000000,");

    const uint64_t boundary_rates[WORKER_COUNT] = {UINT64_MAX, 0, 1, 2};
    const uint64_t boundary_compute[WORKER_COUNT] = {1000000, 0, 1, 2};
    char *boundaries[] = {"static-placement", "--cpus=0,0,1,1", "--iterations=1000",
        "--rates=18446744073709551615,0,1,2", "--computes=1000000,0,1,2", NULL};
    check_options((int)(sizeof(boundaries) / sizeof(boundaries[0]) - 1), boundaries,
        boundary_rates, boundary_compute, uneven_json);
    return EXIT_SUCCESS;
}
