/* Enable Linux/glibc extensions such as the pthread affinity APIs. */
#define _GNU_SOURCE

#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <linux/perf_event.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#define WORKER_COUNT 4
#define PMU_EVENT_COUNT 16

/* Linux encodes the cache, operation, and access/miss selector in separate bytes. */
#define CACHE_EVENT_CONFIG(cache, operation, result) \
    ((uint64_t)(cache) | ((uint64_t)(operation) << 8) | ((uint64_t)(result) << 16))

/// <summary>Run options shared read-only by the main thread and workers.</summary>
struct benchmark_options {
    /// <summary>Logical CPU slots in A0, A1, B0, B1 order.</summary>
    int logical_cpu_slots[WORKER_COUNT];

    /// <summary>Whether to place threads 2 and 3 together on physical core A.</summary>
    bool colocate_sharing_threads;

    /// <summary>Whether to separate counters 2 and 3 onto different cache lines.</summary>
    bool separate_sharing_counters;

    /// <summary>Number of counter increments each worker must complete.</summary>
    uint64_t increments_per_worker;

    /// <summary>Target increments per second per worker, or zero for unpaced execution.</summary>
    uint64_t target_increments_per_second;

    /// <summary>Synthetic integer-compute steps performed after each increment.</summary>
    uint64_t compute_steps_per_increment;

    /// <summary>Maximum increments per batch before optional pacing or idle waits.</summary>
    uint64_t increments_per_batch;

    /// <summary>Requested sleep duration between batches, in microseconds.</summary>
    uint64_t idle_microseconds_per_batch;

    /// <summary>Text file to append JSON results to, or NULL to use stdout.</summary>
    const char *results_file_path;

    /// <summary>Whether to request user-space hardware counters for each worker.</summary>
    bool collect_pmu_counters;

    /// <summary>Whether to use the native counters verified on the Intel N150.</summary>
    bool use_intel_n150_pmu;
};

/// <summary>One Linux perf event requested by the benchmark.</summary>
struct pmu_event_definition {
    /// <summary>Stable JSON name for the counter.</summary>
    const char *name;

    /// <summary>Linux perf event category.</summary>
    uint32_t type;

    /// <summary>Linux event selector within the category.</summary>
    uint64_t config;

    /// <summary>Additional event configuration, including an offcore response filter.</summary>
    uint64_t config1;
};

/// <summary>One counter's handle, measurement, and explicit failure state.</summary>
struct pmu_counter_result {
    /// <summary>Open perf descriptor, or -1 when opening failed.</summary>
    int file_descriptor;

    /// <summary>Descriptor of this counter's scheduling-group leader.</summary>
    int group_leader_descriptor;

    /// <summary>Failed operation or validation, or NULL for a usable measurement.</summary>
    const char *failure_reason;

    /// <summary>Saved errno for a failed system call, or zero for a validation failure.</summary>
    int error_code;

    /// <summary>Unscaled event count read from Linux.</summary>
    uint64_t raw_count;

    /// <summary>Time Linux reports this event was enabled, in nanoseconds.</summary>
    uint64_t enabled_nanoseconds;

    /// <summary>Time Linux reports this event actually ran on the PMU, in nanoseconds.</summary>
    uint64_t running_nanoseconds;
};

/// <summary>Optional CPU frequency-policy observations outside the timed workload.</summary>
struct cpu_frequency_snapshot {
    /// <summary>Frequency governor reported by sysfs.</summary>
    char governor[64];

    /// <summary>Error reading the governor, or zero on success.</summary>
    int governor_error;

    /// <summary>Instantaneous frequency reported by sysfs, in kHz.</summary>
    char frequency_khz[64];

    /// <summary>Error reading the frequency, or zero on success.</summary>
    int frequency_error;
};

/* Adjacent pairs are scheduled together so IPC and access-minus-miss estimates
   do not combine events measured during different multiplexing windows. */
static const struct pmu_event_definition pmu_events[PMU_EVENT_COUNT] = {
    {"cycles", PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES, 0},
    {"instructions", PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS, 0},
    {"branches", PERF_TYPE_HARDWARE, PERF_COUNT_HW_BRANCH_INSTRUCTIONS, 0},
    {"branch_misses", PERF_TYPE_HARDWARE, PERF_COUNT_HW_BRANCH_MISSES, 0},
    {"cache_references", PERF_TYPE_HARDWARE, PERF_COUNT_HW_CACHE_REFERENCES, 0},
    {"cache_misses", PERF_TYPE_HARDWARE, PERF_COUNT_HW_CACHE_MISSES, 0},
    {"l1d_load_accesses", PERF_TYPE_HW_CACHE, CACHE_EVENT_CONFIG(PERF_COUNT_HW_CACHE_L1D, PERF_COUNT_HW_CACHE_OP_READ, PERF_COUNT_HW_CACHE_RESULT_ACCESS), 0},
    {"l1d_load_misses", PERF_TYPE_HW_CACHE, CACHE_EVENT_CONFIG(PERF_COUNT_HW_CACHE_L1D, PERF_COUNT_HW_CACHE_OP_READ, PERF_COUNT_HW_CACHE_RESULT_MISS), 0},
    {"llc_load_accesses", PERF_TYPE_HW_CACHE, CACHE_EVENT_CONFIG(PERF_COUNT_HW_CACHE_LL, PERF_COUNT_HW_CACHE_OP_READ, PERF_COUNT_HW_CACHE_RESULT_ACCESS), 0},
    {"llc_load_misses", PERF_TYPE_HW_CACHE, CACHE_EVENT_CONFIG(PERF_COUNT_HW_CACHE_LL, PERF_COUNT_HW_CACHE_OP_READ, PERF_COUNT_HW_CACHE_RESULT_MISS), 0},
    {"dtlb_load_accesses", PERF_TYPE_HW_CACHE, CACHE_EVENT_CONFIG(PERF_COUNT_HW_CACHE_DTLB, PERF_COUNT_HW_CACHE_OP_READ, PERF_COUNT_HW_CACHE_RESULT_ACCESS), 0},
    {"dtlb_load_misses", PERF_TYPE_HW_CACHE, CACHE_EVENT_CONFIG(PERF_COUNT_HW_CACHE_DTLB, PERF_COUNT_HW_CACHE_OP_READ, PERF_COUNT_HW_CACHE_RESULT_MISS), 0},
    {"dtlb_store_accesses", PERF_TYPE_HW_CACHE, CACHE_EVENT_CONFIG(PERF_COUNT_HW_CACHE_DTLB, PERF_COUNT_HW_CACHE_OP_WRITE, PERF_COUNT_HW_CACHE_RESULT_ACCESS), 0},
    {"dtlb_store_misses", PERF_TYPE_HW_CACHE, CACHE_EVENT_CONFIG(PERF_COUNT_HW_CACHE_DTLB, PERF_COUNT_HW_CACHE_OP_WRITE, PERF_COUNT_HW_CACHE_RESULT_MISS), 0},
    {"itlb_accesses", PERF_TYPE_HW_CACHE, CACHE_EVENT_CONFIG(PERF_COUNT_HW_CACHE_ITLB, PERF_COUNT_HW_CACHE_OP_READ, PERF_COUNT_HW_CACHE_RESULT_ACCESS), 0},
    {"itlb_misses", PERF_TYPE_HW_CACHE, CACHE_EVENT_CONFIG(PERF_COUNT_HW_CACHE_ITLB, PERF_COUNT_HW_CACHE_OP_READ, PERF_COUNT_HW_CACHE_RESULT_MISS), 0}
};

/* Encodings come from the device's perf list --details and verified perf stat runs. */
static const struct pmu_event_definition intel_n150_pmu_events[] = {
    {"mem_load_uops_retired.l1_miss", PERF_TYPE_RAW, UINT64_C(0x08d1), 0},
    {"itlb_misses.walk_completed", PERF_TYPE_RAW, UINT64_C(0x0e85), 0},
    {"ocr.demand_rfo.l3_hit.snoop_hitm", PERF_TYPE_RAW, UINT64_C(0x01b7), UINT64_C(0x10003c0002)}
};

/// <summary>Inputs and results for one worker, kept alive until its thread is joined.</summary>
struct worker_state {
    /// <summary>Shared run options, unchanged after the worker starts.</summary>
    const struct benchmark_options *options;

    /// <summary>Shared rendezvous for setup, timed start, and workload completion.</summary>
    pthread_barrier_t *phase_barrier;

    /// <summary>Common monotonic start timestamp, published through the start barrier.</summary>
    const uint64_t *benchmark_start_nanoseconds;

    /// <summary>This worker's distinct atomic counter in the aligned allocation.</summary>
    atomic_uint_fast64_t *operation_counter;

    /// <summary>Linux thread ID for correlating this worker with profiling samples.</summary>
    long linux_thread_id;

    /// <summary>Logical CPU to which this worker is pinned.</summary>
    int assigned_logical_cpu;

    /// <summary>CPU observed after setting affinity and before the workload.</summary>
    int observed_start_cpu;

    /// <summary>CPU observed after completing the workload.</summary>
    int observed_end_cpu;

    /// <summary>CPU time consumed during the workload, excluding blocked time.</summary>
    uint64_t consumed_cpu_nanoseconds;

    /// <summary>Elapsed time from the common start until this worker completes.</summary>
    uint64_t completion_nanoseconds;

    /// <summary>Per-worker compute state reported so its calculation remains observable.</summary>
    uint64_t compute_checksum;

    /// <summary>Hardware counters and their availability states when PMU collection is requested.</summary>
    struct pmu_counter_result pmu_counters[PMU_EVENT_COUNT];

    /// <summary>Voluntary context switches around the workload, including pacing sleeps.</summary>
    long voluntary_context_switches;

    /// <summary>Involuntary context switches around the workload.</summary>
    long involuntary_context_switches;

    /// <summary>Minor page faults around the workload.</summary>
    long minor_page_faults;

    /// <summary>Major page faults around the workload.</summary>
    long major_page_faults;
};

/* C needs declarations before use; definitions below follow caller-before-callee order.
   main is the exception and remains at the bottom of the file. */
static struct benchmark_options parse_benchmark_options(int argument_count, char **arguments);
static size_t validate_cpu_topology(const struct benchmark_options *options);
static void *run_pinned_worker(void *worker_state_pointer);
static void write_run_metadata(FILE *output_stream, const struct benchmark_options *options, int argument_count, char **arguments, const struct cpu_frequency_snapshot before[WORKER_COUNT], const struct cpu_frequency_snapshot after[WORKER_COUNT]);
static void write_worker_pmu_results(FILE *output_stream, const struct worker_state *worker);
static void open_worker_pmu_counters(struct worker_state *worker);
static const struct pmu_event_definition *get_pmu_event_definition(const struct benchmark_options *options, int event);
static void set_worker_pmu_counters_enabled(struct worker_state *worker, bool enabled);
static void read_and_close_worker_pmu_counters(struct worker_state *worker);
static void capture_cpu_frequency(const struct benchmark_options *options, struct cpu_frequency_snapshot snapshots[WORKER_COUNT]);
static void write_cpu_property_metadata(FILE *output_stream, int logical_cpu, const char *property);
static void write_file_metadata(FILE *output_stream, const char *path);
static void write_optional_metadata_value(FILE *output_stream, const char *value, int error_code);
static void write_pmu_pair_metric(FILE *output_stream, const struct pmu_counter_result *reference, const struct pmu_counter_result *dependent, bool divide_counts);
static void write_json_string(FILE *output_stream, const char *text);
static void wait_for_target_increment_rate(const struct worker_state *worker, uint64_t completed_increments);
static void parse_cpu_slots(const char *input_text, int logical_cpu_slots[WORKER_COUNT]);
static void read_cpu_sysfs_attribute(int logical_cpu, const char *attribute_path, char *attribute_text, size_t attribute_capacity);
static int read_optional_text_file(const char *path, char *text, size_t capacity);
static uint64_t parse_bounded_unsigned_integer(const char *input_text, const char *value_name, uint64_t minimum_value, uint64_t maximum_value);
static void print_command_line_help(FILE *output_stream, const char *executable_name);
static int verify_worker_cpu_affinity(int expected_logical_cpu);
static void sleep_between_work_batches(uint64_t idle_microseconds);
static void wait_for_benchmark_phase(pthread_barrier_t *phase_barrier);
static void require_pthread_success(int error_code, const char *operation_name);
static uint64_t read_clock_nanoseconds(clockid_t clock_id);
static void exit_with_error(const char *error_format, ...);

/// <summary>Builds run options from defaults and validated command-line arguments.</summary>
/// <param name="argument_count">Number of arguments, including the executable name.</param>
/// <param name="arguments">Array of null-terminated argument strings.</param>
/// <returns>The selected options; help or invalid arguments terminate the process.</returns>
static struct benchmark_options parse_benchmark_options(int argument_count, char **arguments)
{
    /* C designated initializers name members; unspecified members start at zero. */
    struct benchmark_options options = {
        .increments_per_worker = 5000000,
        .increments_per_batch = 1000
    };

    bool cpu_slots_provided = false;

    /* The final all-zero entry terminates getopt_long's option table. */
    const struct option long_options[] = {
        {"cpus",       required_argument, NULL, 'c'},
        {"placement",  required_argument, NULL, 'p'},
        {"layout",     required_argument, NULL, 'l'},
        {"iterations", required_argument, NULL, 'n'},
        {"rate",       required_argument, NULL, 'r'},
        {"compute",    required_argument, NULL, 'w'},
        {"batch",      required_argument, NULL, 'b'},
        {"idle-us",    required_argument, NULL, 'i'},
        {"output",     required_argument, NULL, 'o'},
        {"pmu",        no_argument,       NULL, 'm'},
        {"pmu-profile", required_argument, NULL, 'P'},
        {"help",       no_argument,       NULL, 'h'},
        {NULL,         0,                 NULL, 0}
    };

    int option_code;

    while ((option_code = getopt_long(argument_count, arguments, "", long_options, NULL)) != -1) 
    {
        switch (option_code) 
        {
        case 'c':
            parse_cpu_slots(optarg, options.logical_cpu_slots);
            cpu_slots_provided = true;
            break;

        case 'p':
            if (strcmp(optarg, "baseline") != 0 && strcmp(optarg, "colocated") != 0)
            {
                exit_with_error("--placement must be baseline or colocated");
            }

            options.colocate_sharing_threads = strcmp(optarg, "colocated") == 0;
            break;

        case 'l':
            if (strcmp(optarg, "shared") != 0 && strcmp(optarg, "padded") != 0)
            {
                exit_with_error("--layout must be shared or padded");
            }

            options.separate_sharing_counters = strcmp(optarg, "padded") == 0;
            break;

        case 'n':
            options.increments_per_worker = parse_bounded_unsigned_integer(
                optarg, "--iterations", 1, UINT64_MAX / WORKER_COUNT);
            break;

        case 'r':
            options.target_increments_per_second = parse_bounded_unsigned_integer(
                optarg, "--rate", 1, UINT64_MAX);
            break;

        case 'w':
            options.compute_steps_per_increment = parse_bounded_unsigned_integer(
                optarg, "--compute", 0, 1000000);
            break;

        case 'b':
            options.increments_per_batch = parse_bounded_unsigned_integer(
                optarg, "--batch", 1, UINT64_MAX);
            break;

        case 'i':
            options.idle_microseconds_per_batch = parse_bounded_unsigned_integer(
                optarg, "--idle-us", 0, 1000000);
            break;

        case 'o':
            if (optarg[0] == '\0')
            {
                exit_with_error("--output requires a nonempty file path");
            }

            options.results_file_path = optarg;
            break;

        case 'm':
            options.collect_pmu_counters = true;
            break;

        case 'P':
            if (strcmp(optarg, "generic") != 0 && strcmp(optarg, "intel-n150") != 0)
            {
                exit_with_error("--pmu-profile must be generic or intel-n150");
            }

            options.use_intel_n150_pmu = strcmp(optarg, "intel-n150") == 0;
            options.collect_pmu_counters = true;
            break;

        case 'h':
            print_command_line_help(stdout, arguments[0]);
            exit(EXIT_SUCCESS);

        default:
            print_command_line_help(stderr, arguments[0]);
            exit(EXIT_FAILURE);
        }
    }

    if (!cpu_slots_provided || optind != argument_count) 
    {
        print_command_line_help(stderr, arguments[0]);
        exit_with_error("Supply --cpus and no positional arguments");
    }

    if (options.target_increments_per_second != 0 &&
        (long double)options.increments_per_worker * 1000000000.0L /
            (long double)options.target_increments_per_second >= (long double)UINT64_MAX)
    {
        exit_with_error("Requested rate and work exceed the supported timing range");
    }

    if (options.use_intel_n150_pmu)
    {
        char cpu_info[65536];
        int error_code = read_optional_text_file("/proc/cpuinfo", cpu_info, sizeof(cpu_info));

        if (error_code != 0)
        {
            exit_with_error("Cannot verify the Intel N150 PMU profile: %s", strerror(error_code));
        }

        if (strstr(cpu_info, "GenuineIntel") == NULL || strstr(cpu_info, "Intel(R) N150") == NULL)
        {
            exit_with_error("--pmu-profile intel-n150 requires an Intel N150 CPU");
        }
    }

    return options;
}

/// <summary>Validates CPU eligibility, two-core grouping, and a common cache-line size.</summary>
/// <param name="options">Requested logical CPU slots for the experiment.</param>
/// <returns>The selected CPUs' common cache-line size in bytes.</returns>
/// <remarks>Uses Linux-reported topology, which cannot reveal hidden VM placement.</remarks>
static size_t validate_cpu_topology(const struct benchmark_options *options)
{
    cpu_set_t allowed_cpu_set;

    if (sched_getaffinity(0, sizeof(allowed_cpu_set), &allowed_cpu_set) != 0) 
    {
        exit_with_error("sched_getaffinity (CPU_SETSIZE=%d): %s", CPU_SETSIZE, strerror(errno));
    }

    char sibling_cpu_lists[WORKER_COUNT][4096];
    size_t common_cache_line_bytes = 0;

    for (int slot_index = 0; slot_index < WORKER_COUNT; ++slot_index) 
    {
        int logical_cpu = options->logical_cpu_slots[slot_index];

        if (!CPU_ISSET(logical_cpu, &allowed_cpu_set)) 
        {
            exit_with_error("CPU %d is outside the process's allowed CPU set", logical_cpu);
        }

        read_cpu_sysfs_attribute(
            logical_cpu,
            "topology/thread_siblings_list",
            sibling_cpu_lists[slot_index],
            sizeof(sibling_cpu_lists[slot_index]));

        char cache_line_size_text[64];

        read_cpu_sysfs_attribute(
            logical_cpu,
            "cache/index0/coherency_line_size",
            cache_line_size_text,
            sizeof(cache_line_size_text));

        size_t reported_cache_line_bytes = (size_t)parse_bounded_unsigned_integer(
            cache_line_size_text,
            "cache-line size",
            sizeof(atomic_uint_fast64_t) * 2,
            4096);

        /* Clearing the lowest set bit gives zero only for a power of two. */
        if ((reported_cache_line_bytes & (reported_cache_line_bytes - 1)) != 0 || reported_cache_line_bytes < sizeof(void *)) 
        {
            exit_with_error("Unsupported cache-line alignment: %zu", reported_cache_line_bytes);
        }

        if (slot_index != 0 && reported_cache_line_bytes != common_cache_line_bytes) 
        {
            exit_with_error("Selected CPUs have different cache-line sizes");
        }

        common_cache_line_bytes = reported_cache_line_bytes;
    }

    /* Sibling lists identify a core; CPU numbering need not be consecutive. */
    if (strcmp(sibling_cpu_lists[0], sibling_cpu_lists[1]) != 0 ||
        strcmp(sibling_cpu_lists[2], sibling_cpu_lists[3]) != 0 ||
        strcmp(sibling_cpu_lists[0], sibling_cpu_lists[2]) == 0) 
    {
        exit_with_error("--cpus must describe two logical-CPU pairs on distinct physical cores");
    }

    return common_cache_line_bytes;
}

/// <summary>Pins one worker, performs its fixed workload, and records timing and placement.</summary>
/// <param name="worker_state_pointer">Worker state owned by main and kept alive until join.</param>
/// <returns>NULL; results are written to the worker state instead of the thread return value.</returns>
/// <remarks>Called by pthread_create, which requires the generic void-pointer signature.</remarks>
static void *run_pinned_worker(void *worker_state_pointer)
{
    /* C allows conversion from void* to an object pointer without an explicit cast. */
    struct worker_state *worker = worker_state_pointer;
    const struct benchmark_options options = *worker->options;
    atomic_uint_fast64_t *operation_counter = worker->operation_counter;

    worker->linux_thread_id = syscall(SYS_gettid);

    if (worker->linux_thread_id < 0) 
    {
        exit_with_error("gettid: %s", strerror(errno));
    }

    /* cpu_set_t is a bit mask; pthread_setaffinity_np applies it to this worker only. */
    cpu_set_t assigned_cpu_set;
    CPU_ZERO(&assigned_cpu_set);
    CPU_SET(worker->assigned_logical_cpu, &assigned_cpu_set);

    require_pthread_success(
        pthread_setaffinity_np(pthread_self(), sizeof(assigned_cpu_set), &assigned_cpu_set),
        "pthread_setaffinity_np");

    worker->observed_start_cpu = verify_worker_cpu_affinity(worker->assigned_logical_cpu);

    if (options.collect_pmu_counters)
    {
        open_worker_pmu_counters(worker);
    }

    /* First rendezvous: all workers are pinned. Second: main releases timed work. */
    wait_for_benchmark_phase(worker->phase_barrier);
    wait_for_benchmark_phase(worker->phase_barrier);

    struct rusage usage_before;

    if (getrusage(RUSAGE_THREAD, &usage_before) != 0)
    {
        exit_with_error("getrusage before workload: %s", strerror(errno));
    }

    /* Thread CPU time excludes time asleep or waiting to be scheduled. */
    uint64_t starting_cpu_nanoseconds = read_clock_nanoseconds(CLOCK_THREAD_CPUTIME_ID);

    if (options.collect_pmu_counters)
    {
        set_worker_pmu_counters_enabled(worker, true);
    }

    uint64_t compute_checksum = worker->compute_checksum;
    uint64_t remaining_increments = options.increments_per_worker;

    while (remaining_increments != 0) 
    {
        uint64_t batch_increments = remaining_increments < options.increments_per_batch ? remaining_increments : options.increments_per_batch;

        for (uint64_t increment_index = 0; increment_index < batch_increments; ++increment_index) 
        {
            /* Distinct counters can contend through a cache line, not a shared value.
               Relaxed atomics do not impose ordering on unrelated memory accesses. */
            atomic_fetch_add_explicit(operation_counter, 1, memory_order_relaxed);

            for (uint64_t compute_step = 0; compute_step < options.compute_steps_per_increment; ++compute_step) 
            {
                /* Unsigned wraparound is intentional; the printed checksum uses the result. */
                compute_checksum = compute_checksum * UINT64_C(6364136223846793005) + 1;
            }
        }

        remaining_increments -= batch_increments;

        if (options.target_increments_per_second != 0)
        {
            wait_for_target_increment_rate(
                worker,
                options.increments_per_worker - remaining_increments);
        }

        if (remaining_increments != 0 && options.idle_microseconds_per_batch != 0) 
        {
            sleep_between_work_batches(options.idle_microseconds_per_batch);
        }
    }

    /* Capture work time before affinity verification and the final rendezvous. */
    worker->consumed_cpu_nanoseconds = read_clock_nanoseconds(CLOCK_THREAD_CPUTIME_ID) - starting_cpu_nanoseconds;
    worker->completion_nanoseconds = read_clock_nanoseconds(CLOCK_MONOTONIC) - *worker->benchmark_start_nanoseconds;

    if (options.collect_pmu_counters)
    {
        set_worker_pmu_counters_enabled(worker, false);
    }

    struct rusage usage_after;

    if (getrusage(RUSAGE_THREAD, &usage_after) != 0)
    {
        exit_with_error("getrusage after workload: %s", strerror(errno));
    }

    worker->voluntary_context_switches = usage_after.ru_nvcsw - usage_before.ru_nvcsw;
    worker->involuntary_context_switches = usage_after.ru_nivcsw - usage_before.ru_nivcsw;
    worker->minor_page_faults = usage_after.ru_minflt - usage_before.ru_minflt;
    worker->major_page_faults = usage_after.ru_majflt - usage_before.ru_majflt;
    worker->compute_checksum = compute_checksum;
    worker->observed_end_cpu = verify_worker_cpu_affinity(worker->assigned_logical_cpu);

    wait_for_benchmark_phase(worker->phase_barrier);

    if (options.collect_pmu_counters)
    {
        read_and_close_worker_pmu_counters(worker);
    }

    return NULL;
}

/// <summary>Writes portable machine, invocation, topology, and frequency context.</summary>
/// <param name="output_stream">JSON destination selected by main.</param>
/// <param name="options">CPU slots and measurement configuration.</param>
/// <param name="argument_count">Number of invocation arguments.</param>
/// <param name="arguments">Original invocation arguments.</param>
/// <param name="before">Frequency observations before worker creation.</param>
/// <param name="after">Frequency observations after all workers were joined.</param>
static void write_run_metadata(
    FILE *output_stream,
    const struct benchmark_options *options,
    int argument_count,
    char **arguments,
    const struct cpu_frequency_snapshot before[WORKER_COUNT],
    const struct cpu_frequency_snapshot after[WORKER_COUNT])
{
    struct utsname system;
    time_t recorded_time = time(NULL);
    struct tm utc_time;
    char timestamp[32];
    cpu_set_t allowed_cpus;

    if (uname(&system) != 0)
    {
        exit_with_error("uname: %s", strerror(errno));
    }

    if (recorded_time == (time_t)-1 || gmtime_r(&recorded_time, &utc_time) == NULL)
    {
        exit_with_error("Cannot obtain the results' UTC timestamp");
    }

    if (strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &utc_time) == 0)
    {
        exit_with_error("Cannot format the results' UTC timestamp");
    }

    if (sched_getaffinity(0, sizeof(allowed_cpus), &allowed_cpus) != 0)
    {
        exit_with_error("sched_getaffinity for metadata: %s", strerror(errno));
    }

    fputs("{\"recorded_at_utc\":", output_stream);
    write_json_string(output_stream, timestamp);
    fputs(",\"kernel_release\":", output_stream);
    write_json_string(output_stream, system.release);
    fputs(",\"kernel_version\":", output_stream);
    write_json_string(output_stream, system.version);
    fputs(",\"architecture\":", output_stream);
    write_json_string(output_stream, system.machine);
    fputs(",\"host\":", output_stream);
    write_json_string(output_stream, system.nodename);
    fputs(",\"build_timestamp\":", output_stream);
    write_json_string(output_stream, __DATE__ " " __TIME__);
    fputs(",\"arguments\":[", output_stream);

    for (int argument_index = 0; argument_index < argument_count; ++argument_index)
    {
        if (argument_index != 0)
        {
            fputc(',', output_stream);
        }

        write_json_string(output_stream, arguments[argument_index]);
    }

    fputs("],\"allowed_cpus\":[", output_stream);
    bool first_cpu = true;

    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu)
    {
        if (CPU_ISSET(cpu, &allowed_cpus))
        {
            fprintf(output_stream, "%s%d", first_cpu ? "" : ",", cpu);
            first_cpu = false;
        }
    }

    fputs("],\"cpu_info\":", output_stream);
    write_file_metadata(output_stream, "/proc/cpuinfo");
    fputs(",\"online_cpus\":", output_stream);
    write_file_metadata(output_stream, "/sys/devices/system/cpu/online");
    fputs(",\"smt_active\":", output_stream);
    write_file_metadata(output_stream, "/sys/devices/system/cpu/smt/active");
    fputs(",\"perf_event_paranoid\":", output_stream);
    write_file_metadata(output_stream, "/proc/sys/kernel/perf_event_paranoid");
    fprintf(output_stream, ",\"pmu_requested\":%s,\"pmu_profile\":\"%s\",\"cpu_slots\":[",
        options->collect_pmu_counters ? "true" : "false",
        options->use_intel_n150_pmu ? "intel-n150" : "generic");

    for (int slot = 0; slot < WORKER_COUNT; ++slot)
    {
        int cpu = options->logical_cpu_slots[slot];
        fprintf(output_stream, "%s{\"cpu\":%d,\"package_id\":", slot == 0 ? "" : ",", cpu);
        write_cpu_property_metadata(output_stream, cpu, "topology/physical_package_id");
        fputs(",\"core_id\":", output_stream);
        write_cpu_property_metadata(output_stream, cpu, "topology/core_id");
        fputs(",\"thread_siblings\":", output_stream);
        write_cpu_property_metadata(output_stream, cpu, "topology/thread_siblings_list");
        fputs(",\"minimum_frequency_khz\":", output_stream);
        write_cpu_property_metadata(output_stream, cpu, "cpufreq/scaling_min_freq");
        fputs(",\"maximum_frequency_khz\":", output_stream);
        write_cpu_property_metadata(output_stream, cpu, "cpufreq/scaling_max_freq");
        fputs(",\"governor_before\":", output_stream);
        write_optional_metadata_value(output_stream, before[slot].governor, before[slot].governor_error);
        fputs(",\"governor_after\":", output_stream);
        write_optional_metadata_value(output_stream, after[slot].governor, after[slot].governor_error);
        fputs(",\"frequency_khz_before\":", output_stream);
        write_optional_metadata_value(output_stream, before[slot].frequency_khz, before[slot].frequency_error);
        fputs(",\"frequency_khz_after\":", output_stream);
        write_optional_metadata_value(output_stream, after[slot].frequency_khz, after[slot].frequency_error);
        fputc('}', output_stream);
    }

    fputs("]}", output_stream);
}

/// <summary>Writes counter counts, multiplexing coverage, failures, and derived estimates.</summary>
/// <param name="output_stream">JSON destination selected by main.</param>
/// <param name="worker">Joined worker whose counters are no longer running.</param>
static void write_worker_pmu_results(FILE *output_stream, const struct worker_state *worker)
{
    if (!worker->options->collect_pmu_counters)
    {
        fputs("null", output_stream);
        return;
    }

    fprintf(output_stream, "{\"scope\":\"thread_user_space\",\"profile\":\"%s\",\"events\":{",
        worker->options->use_intel_n150_pmu ? "intel-n150" : "generic");

    for (int event = 0; event < PMU_EVENT_COUNT; ++event)
    {
        const struct pmu_counter_result *counter = &worker->pmu_counters[event];
        const struct pmu_event_definition *definition = get_pmu_event_definition(worker->options, event);

        if (event != 0)
        {
            fputc(',', output_stream);
        }

        write_json_string(output_stream, definition->name);
        fprintf(output_stream,
            ":{\"type\":%" PRIu32 ",\"config\":%" PRIu64 ",\"config1\":%" PRIu64 ",\"group\":%d,",
            definition->type, definition->config, definition->config1, event / 2);

        if (counter->failure_reason != NULL)
        {
            fputs("\"status\":\"unavailable\",\"operation\":", output_stream);
            write_json_string(output_stream, counter->failure_reason);
            fprintf(output_stream, ",\"errno\":%d,\"reason\":", counter->error_code);
            write_json_string(output_stream, counter->error_code != 0
                ? strerror(counter->error_code) : counter->failure_reason);
            fputs(",\"raw_count\":null,\"scaled_count\":null,"
                "\"time_enabled_ns\":null,\"time_running_ns\":null,\"running_fraction\":null}", output_stream);
        }
        else
        {
            double scale = (double)counter->enabled_nanoseconds / (double)counter->running_nanoseconds;
            fprintf(output_stream,
                "\"status\":\"ok\",\"raw_count\":%" PRIu64 ",\"scaled_count\":%.3f,"
                "\"time_enabled_ns\":%" PRIu64 ",\"time_running_ns\":%" PRIu64 ","
                "\"running_fraction\":%.6f}",
                counter->raw_count, (double)counter->raw_count * scale,
                counter->enabled_nanoseconds, counter->running_nanoseconds, 1.0 / scale);
        }
    }

    fputs("},\"ipc_estimate\":", output_stream);
    write_pmu_pair_metric(output_stream, &worker->pmu_counters[0], &worker->pmu_counters[1], true);
    fputs(",\"l1d_load_hits_estimate\":", output_stream);

    if (worker->options->use_intel_n150_pmu)
    {
        fputs("{\"value\":null,\"reason\":\"Generic L1D accesses and native retired-load misses are not a matched hit-count pair\"}",
            output_stream);
    }
    else
    {
        write_pmu_pair_metric(output_stream, &worker->pmu_counters[6], &worker->pmu_counters[7], false);
    }

    fputs(",\"llc_load_hits_estimate\":", output_stream);
    write_pmu_pair_metric(output_stream, &worker->pmu_counters[8], &worker->pmu_counters[9], false);
    fputc('}', output_stream);
}

/// <summary>Opens disabled, per-thread counters in small jointly scheduled pairs.</summary>
/// <param name="worker">Calling worker whose supported events will be measured.</param>
/// <remarks>Opening failures are retained for JSON reporting, not substituted with counts.</remarks>
static void open_worker_pmu_counters(struct worker_state *worker)
{
    for (int event = 0; event < PMU_EVENT_COUNT; ++event)
    {
        struct pmu_counter_result *counter = &worker->pmu_counters[event];
        const struct pmu_event_definition *definition = get_pmu_event_definition(worker->options, event);
        int leader = event % 2 != 0 ? worker->pmu_counters[event - 1].file_descriptor : -1;
        struct perf_event_attr attributes = {
            .type = definition->type,
            .size = sizeof(struct perf_event_attr),
            .config = definition->config,
            .config1 = definition->config1,
            .read_format = PERF_FORMAT_GROUP |
                PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING,
            .disabled = leader < 0,
            .exclude_kernel = 1,
            .exclude_hv = 1
        };

        /* pid=0/cpu=-1 follows this calling thread, not every task on its chosen CPU. */
        counter->file_descriptor = (int)syscall(
            SYS_perf_event_open, &attributes, 0, -1, leader, PERF_FLAG_FD_CLOEXEC);
        counter->group_leader_descriptor = leader >= 0 ? leader : counter->file_descriptor;

        if (counter->file_descriptor < 0)
        {
            counter->failure_reason = "perf_event_open";
            counter->error_code = errno;
        }
    }
}

/// <summary>Selects a generic counter or an explicitly requested Intel N150 native counter.</summary>
/// <param name="options">Validated run options containing the selected profile.</param>
/// <param name="event">Counter slot in the fixed adjacent-pair layout.</param>
/// <returns>The definition used consistently for collection and JSON reporting.</returns>
static const struct pmu_event_definition *get_pmu_event_definition(
    const struct benchmark_options *options,
    int event)
{
    if (options->use_intel_n150_pmu)
    {
        switch (event)
        {
        case 7:
            return &intel_n150_pmu_events[0];

        case 14:
            return &intel_n150_pmu_events[1];

        case 15:
            return &intel_n150_pmu_events[2];
        }
    }

    return &pmu_events[event];
}

/// <summary>Starts or stops each successfully opened counter group.</summary>
/// <param name="worker">Calling worker with open counters.</param>
/// <param name="enabled">True to start counting; false to stop counting.</param>
static void set_worker_pmu_counters_enabled(struct worker_state *worker, bool enabled)
{
    for (int event = 0; event < PMU_EVENT_COUNT; ++event)
    {
        struct pmu_counter_result *leader = &worker->pmu_counters[event];

        if (leader->file_descriptor < 0 || leader->file_descriptor != leader->group_leader_descriptor)
        {
            continue;
        }

        if (ioctl(leader->file_descriptor,
                enabled ? PERF_EVENT_IOC_ENABLE : PERF_EVENT_IOC_DISABLE,
                (unsigned long)PERF_IOC_FLAG_GROUP) != 0)
        {
            int error_code = errno;

            for (int member = 0; member < PMU_EVENT_COUNT; ++member)
            {
                struct pmu_counter_result *counter = &worker->pmu_counters[member];

                if (counter->group_leader_descriptor == leader->file_descriptor &&
                    counter->failure_reason == NULL)
                {
                    counter->failure_reason = enabled ? "enable" : "disable";
                    counter->error_code = error_code;
                }
            }
        }
    }
}

/// <summary>Reads stopped counters, validates their coverage, and releases all descriptors.</summary>
/// <param name="worker">Calling worker after the measured workload and finish barrier.</param>
static void read_and_close_worker_pmu_counters(struct worker_state *worker)
{
    for (int event = 0; event < PMU_EVENT_COUNT; ++event)
    {
        struct pmu_counter_result *leader = &worker->pmu_counters[event];

        if (leader->file_descriptor < 0 ||
            leader->file_descriptor != leader->group_leader_descriptor ||
            leader->failure_reason != NULL)
        {
            continue;
        }

        size_t member_count = 1;

        if (event + 1 < PMU_EVENT_COUNT &&
            worker->pmu_counters[event + 1].file_descriptor >= 0 &&
            worker->pmu_counters[event + 1].group_leader_descriptor == leader->file_descriptor)
        {
            member_count = 2;
        }

        /* GROUP returns nr, time_enabled, time_running, then leader/partner counts.
           One read gives both members the same counting window and coverage. */
        uint64_t reading[5];
        ssize_t bytes_read;

        do
        {
            bytes_read = read(leader->file_descriptor, reading, sizeof(reading));
        }
        while (bytes_read < 0 && errno == EINTR);

        const char *failure_reason = NULL;
        int error_code = 0;

        if (bytes_read != (ssize_t)((3 + member_count) * sizeof(uint64_t)))
        {
            failure_reason = "incomplete counter group read";
            error_code = bytes_read < 0 ? errno : 0;
        }
        else if (reading[0] != member_count)
        {
            failure_reason = "unexpected counter group size";
        }
        else if (reading[2] == 0)
        {
            failure_reason = "event group was never scheduled on the PMU";
        }
        else if (reading[2] > reading[1])
        {
            failure_reason = "counter running time exceeds enabled time";
        }

        for (size_t member = 0; member < member_count; ++member)
        {
            struct pmu_counter_result *counter = &worker->pmu_counters[event + (int)member];

            if (failure_reason != NULL)
            {
                counter->failure_reason = failure_reason;
                counter->error_code = error_code;
            }
            else
            {
                counter->raw_count = reading[3 + member];
                counter->enabled_nanoseconds = reading[1];
                counter->running_nanoseconds = reading[2];
            }
        }
    }

    /* Keep groups intact until every member's stopped reading has been collected. */
    for (int event = 0; event < PMU_EVENT_COUNT; ++event)
    {
        struct pmu_counter_result *counter = &worker->pmu_counters[event];

        if (counter->file_descriptor >= 0)
        {
            if (close(counter->file_descriptor) != 0 && counter->failure_reason == NULL)
            {
                counter->failure_reason = "close";
                counter->error_code = errno;
            }

            counter->file_descriptor = -1;
        }
    }
}

/// <summary>Captures optional governor and instantaneous frequency values for each CPU slot.</summary>
/// <param name="options">CPU slots whose frequency policy is observed.</param>
/// <param name="snapshots">Caller-owned array receiving values and read errors.</param>
static void capture_cpu_frequency(
    const struct benchmark_options *options,
    struct cpu_frequency_snapshot snapshots[WORKER_COUNT])
{
    for (int slot = 0; slot < WORKER_COUNT; ++slot)
    {
        char path[256];
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_governor",
            options->logical_cpu_slots[slot]);
        snapshots[slot].governor_error = read_optional_text_file(
            path, snapshots[slot].governor, sizeof(snapshots[slot].governor));
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq",
            options->logical_cpu_slots[slot]);
        snapshots[slot].frequency_error = read_optional_text_file(
            path, snapshots[slot].frequency_khz, sizeof(snapshots[slot].frequency_khz));
    }
}

/// <summary>Writes an optional sysfs CPU property as a value/error object.</summary>
/// <param name="output_stream">JSON destination.</param>
/// <param name="logical_cpu">CPU whose sysfs directory is inspected.</param>
/// <param name="property">Constant property path within that directory.</param>
static void write_cpu_property_metadata(FILE *output_stream, int logical_cpu, const char *property)
{
    char path[256];
    int path_length = snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/%s", logical_cpu, property);

    if (path_length < 0 || (size_t)path_length >= sizeof(path))
    {
        exit_with_error("CPU metadata path is too long");
    }

    write_file_metadata(output_stream, path);
}

/// <summary>Writes a bounded text-file observation without hiding unavailable metadata.</summary>
/// <param name="output_stream">JSON destination.</param>
/// <param name="path">Metadata file to read outside the measured workload.</param>
static void write_file_metadata(FILE *output_stream, const char *path)
{
    char text[65536];
    int error_code = read_optional_text_file(path, text, sizeof(text));
    write_optional_metadata_value(output_stream, text, error_code);
}

/// <summary>Writes either an escaped metadata value or a null value with its read error.</summary>
/// <param name="output_stream">JSON destination.</param>
/// <param name="value">Text obtained from the metadata file.</param>
/// <param name="error_code">Zero on success, or an explicit read error.</param>
static void write_optional_metadata_value(FILE *output_stream, const char *value, int error_code)
{
    fputs("{\"value\":", output_stream);

    if (error_code == 0)
    {
        write_json_string(output_stream, value);
        fputs(",\"error\":null}", output_stream);
    }
    else
    {
        fprintf(output_stream, "null,\"errno\":%d,\"error\":", error_code);
        write_json_string(output_stream, strerror(error_code));
        fputc('}', output_stream);
    }
}

/// <summary>Writes a paired IPC ratio or scaled access-minus-miss estimate.</summary>
/// <param name="output_stream">JSON destination.</param>
/// <param name="reference">Cycles for IPC, or accesses for estimated cache hits.</param>
/// <param name="dependent">Instructions for IPC, or misses for estimated cache hits.</param>
/// <param name="divide_counts">True for instructions/cycles; false for accesses minus misses.</param>
static void write_pmu_pair_metric(
    FILE *output_stream,
    const struct pmu_counter_result *reference,
    const struct pmu_counter_result *dependent,
    bool divide_counts)
{
    const char *unavailable_reason = NULL;

    if (reference->failure_reason != NULL || dependent->failure_reason != NULL ||
        reference->group_leader_descriptor != dependent->group_leader_descriptor)
    {
        unavailable_reason = "both counters must be usable and jointly scheduled";
    }
    else if (divide_counts && reference->raw_count == 0)
    {
        unavailable_reason = "cycle count is zero";
    }
    else if (!divide_counts && dependent->raw_count > reference->raw_count)
    {
        unavailable_reason = "misses exceed accesses; hit subtraction is not valid";
    }

    fputs("{\"value\":", output_stream);

    if (unavailable_reason != NULL)
    {
        fputs("null,\"reason\":", output_stream);
        write_json_string(output_stream, unavailable_reason);
        fputc('}', output_stream);
        return;
    }

    double value = divide_counts
        ? (double)dependent->raw_count / (double)reference->raw_count
        : (double)(reference->raw_count - dependent->raw_count) *
            ((double)reference->enabled_nanoseconds / (double)reference->running_nanoseconds);
    fprintf(output_stream, "%.6f,\"reason\":null}", value);
}

/// <summary>Writes one JSON string with quotes, backslashes, and control characters escaped.</summary>
/// <param name="output_stream">JSON destination.</param>
/// <param name="text">Null-terminated text to encode.</param>
static void write_json_string(FILE *output_stream, const char *text)
{
    fputc('"', output_stream);

    for (const unsigned char *character = (const unsigned char *)text; *character != '\0'; ++character)
    {
        if (*character == '"' || *character == '\\')
        {
            fputc('\\', output_stream);
            fputc(*character, output_stream);
        }
        else if (*character < 0x20)
        {
            fprintf(output_stream, "\\u%04x", (unsigned int)*character);
        }
        else
        {
            fputc(*character, output_stream);
        }
    }

    fputc('"', output_stream);
}

/// <summary>Waits only when a completed batch is ahead of its requested rate schedule.</summary>
/// <param name="worker">Worker with a nonzero rate target and a published common start.</param>
/// <param name="completed_increments">Total increments completed by this worker so far.</param>
/// <remarks>
/// Absolute deadlines avoid accumulating sleep drift. Late workers do not sleep or drop work.
/// The final batch is paced too, so a fast worker does not finish before its nominal duration.
/// </remarks>
static void wait_for_target_increment_rate(const struct worker_state *worker, uint64_t completed_increments)
{
    /* Convert before multiplying so a large operation count cannot overflow an integer.
       Long double retains precision for the requested workload's nanosecond deadlines. */
    long double target_elapsed_nanoseconds =
        (long double)completed_increments * 1000000000.0L /
        (long double)worker->options->target_increments_per_second;

    uint64_t start_nanoseconds = *worker->benchmark_start_nanoseconds;

    if (target_elapsed_nanoseconds >= (long double)(UINT64_MAX - start_nanoseconds))
    {
        exit_with_error("Rate-control deadline exceeds the supported timing range");
    }

    uint64_t deadline_nanoseconds = start_nanoseconds + (uint64_t)target_elapsed_nanoseconds;

    if (read_clock_nanoseconds(CLOCK_MONOTONIC) >= deadline_nanoseconds)
    {
        return;
    }

    struct timespec deadline =
    {
        .tv_sec = (time_t)(deadline_nanoseconds / UINT64_C(1000000000)),
        .tv_nsec = (long)(deadline_nanoseconds % UINT64_C(1000000000))
    };

    if ((uint64_t)deadline.tv_sec != deadline_nanoseconds / UINT64_C(1000000000))
    {
        exit_with_error("Rate-control deadline cannot be represented by time_t");
    }

    int sleep_error;

    /* Retrying the same absolute deadline avoids extending a sleep after a signal. */
    do
    {
        sleep_error = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, NULL);
    }
    while (sleep_error == EINTR);

    if (sleep_error != 0)
    {
        exit_with_error("clock_nanosleep: %s", strerror(sleep_error));
    }
}

/// <summary>Parses exactly four comma-separated logical CPU IDs into the slot array.</summary>
/// <param name="input_text">CPU IDs in A0, A1, B0, B1 order.</param>
/// <param name="logical_cpu_slots">Caller-owned output array containing four CPU slots.</param>
/// <remarks>This validates syntax and mask bounds; topology is checked separately.</remarks>
static void parse_cpu_slots(const char *input_text, int logical_cpu_slots[WORKER_COUNT])
{
    for (int slot_index = 0; slot_index < WORKER_COUNT; ++slot_index) 
    {
        char *unparsed_suffix;

        errno = 0;

        unsigned long parsed_value = strtoul(input_text, &unparsed_suffix, 10);
        char expected_separator = slot_index == WORKER_COUNT - 1 ? '\0' : ',';

        if (input_text[0] < '0' || input_text[0] > '9' || errno == ERANGE ||
            parsed_value >= CPU_SETSIZE || *unparsed_suffix != expected_separator) 
        {
            exit_with_error("--cpus requires exactly A0,A1,B0,B1, each from 0 to %d", CPU_SETSIZE - 1);
        }

        logical_cpu_slots[slot_index] = (int)parsed_value;

        if (expected_separator != '\0') 
        {
            input_text = unparsed_suffix + 1;
        }
    }
}

/// <summary>Reads one complete sysfs CPU attribute and removes its trailing newline.</summary>
/// <param name="logical_cpu">Logical CPU whose sysfs directory is used.</param>
/// <param name="attribute_path">Attribute path relative to that CPU directory.</param>
/// <param name="attribute_text">Caller-owned buffer receiving a null-terminated string.</param>
/// <param name="attribute_capacity">Buffer size in bytes, including space for the terminator.</param>
/// <remarks>Missing, unreadable, or truncated attributes terminate the run.</remarks>
static void read_cpu_sysfs_attribute(
    int logical_cpu,
    const char *attribute_path,
    char *attribute_text,
    size_t attribute_capacity)
{
    char sysfs_path[256];

    int path_length = snprintf(
        sysfs_path,
        sizeof(sysfs_path),
        "/sys/devices/system/cpu/cpu%d/%s",
        logical_cpu,
        attribute_path);

    if (path_length < 0 || (size_t)path_length >= sizeof(sysfs_path)) {
        exit_with_error("CPU property path is too long");
    }

    FILE *attribute_file = fopen(sysfs_path, "r");

    if (attribute_file == NULL) {
        exit_with_error("%s: %s", sysfs_path, strerror(errno));
    }

    if (fgets(attribute_text, (int)attribute_capacity, attribute_file) == NULL) {
        exit_with_error("Cannot read %s", sysfs_path);
    }

    if (strchr(attribute_text, '\n') == NULL || ferror(attribute_file)) {
        exit_with_error("Incomplete CPU property: %s", sysfs_path);
    }

    if (fclose(attribute_file) != 0) {
        exit_with_error("Cannot close %s: %s", sysfs_path, strerror(errno));
    }

    attribute_text[strcspn(attribute_text, "\n")] = '\0';
}

/// <summary>Reads a complete bounded text file, returning an explicit error when unavailable.</summary>
/// <param name="path">Metadata file to read.</param>
/// <param name="text">Caller-owned buffer receiving null-terminated text.</param>
/// <param name="capacity">Buffer size including the terminating null.</param>
/// <returns>Zero on success, or an errno-style code; errors leave an empty buffer.</returns>
static int read_optional_text_file(const char *path, char *text, size_t capacity)
{
    text[0] = '\0';
    FILE *file = fopen(path, "r");

    if (file == NULL)
    {
        return errno;
    }

    errno = 0;
    size_t bytes_read = fread(text, 1, capacity - 1, file);
    int error_code = ferror(file) ? (errno != 0 ? errno : EIO) : 0;

    if (error_code == 0 && bytes_read == capacity - 1)
    {
        if (fgetc(file) != EOF)
        {
            error_code = EOVERFLOW;
        }
        else if (ferror(file))
        {
            error_code = errno != 0 ? errno : EIO;
        }
    }

    if (error_code == 0 && memchr(text, '\0', bytes_read) != NULL)
    {
        error_code = EILSEQ;
    }

    if (fclose(file) != 0 && error_code == 0)
    {
        error_code = errno;
    }

    text[bytes_read] = '\0';

    if (error_code != 0)
    {
        text[0] = '\0';
    }
    else
    {
        while (bytes_read != 0 && (text[bytes_read - 1] == '\n' || text[bytes_read - 1] == '\r'))
        {
            text[--bytes_read] = '\0';
        }
    }

    return error_code;
}

/// <summary>Parses a complete unsigned decimal value and enforces inclusive bounds.</summary>
/// <param name="input_text">The null-terminated number to parse.</param>
/// <param name="value_name">Option or attribute name used in an error message.</param>
/// <param name="minimum_value">Smallest accepted value.</param>
/// <param name="maximum_value">Largest accepted value.</param>
/// <returns>The validated value, or terminates the run for invalid input.</returns>
static uint64_t parse_bounded_unsigned_integer(
    const char *input_text,
    const char *value_name,
    uint64_t minimum_value,
    uint64_t maximum_value)
{
    char *unparsed_suffix;

    errno = 0;

    /* The suffix starts at the first unconsumed character; '\0' means no text is left. */
    unsigned long long parsed_value = strtoull(input_text, &unparsed_suffix, 10);

    if (input_text[0] < '0' || input_text[0] > '9' || *unparsed_suffix != '\0' ||
        errno == ERANGE || parsed_value < minimum_value || parsed_value > maximum_value)
    {
        exit_with_error(
            "%s must be an integer from %" PRIu64 " to %" PRIu64,
            value_name,
            minimum_value,
            maximum_value);
    }

    return (uint64_t)parsed_value;
}

/// <summary>Prints command-line options without starting a workload.</summary>
/// <param name="output_stream">stdout for requested help, or stderr for usage errors.</param>
/// <param name="executable_name">Executable name shown in the usage line.</param>
static void print_command_line_help(FILE *output_stream, const char *executable_name)
{
    fprintf(
        output_stream,
        "Usage: %s --cpus A0,A1,B0,B1 [options]\n"
        "  A0/A1 must belong to one physical core; B0/B1 to another.\n"
        "  Repeat a CPU within a pair to time-share one logical CPU.\n"
        "  --placement baseline|colocated  Default: baseline\n"
        "  --layout shared|padded          Default: shared\n"
        "  --iterations N                  Increments per worker (5000000)\n"
        "  --rate N                        Target increments/s per worker (unpaced)\n"
        "                                  Finish all work even when behind target\n"
        "  --compute N                     Compute steps per increment (0)\n"
        "  --batch N                       Increments per batch (1000)\n"
        "  --idle-us N                     Idle time between batches (0)\n"
        "  --output PATH                   Append JSON results to a text file\n"
        "                                  Default: stdout; existing files are kept\n"
        "  --pmu                           Request per-thread hardware counters\n"
        "                                  Unavailable counters have explicit reasons\n"
        "  --pmu-profile generic|intel-n150 Select counters and enable --pmu\n"
        "                                  Default: generic; native profile requires N150\n"
        "  --help                          Show this help\n",
        executable_name);
}

/// <summary>Checks that the calling worker is restricted to and running on its chosen CPU.</summary>
/// <param name="expected_logical_cpu">The single logical CPU permitted for this worker.</param>
/// <returns>The CPU observed by sched_getcpu after verifying the affinity mask.</returns>
/// <remarks>This is an endpoint check, not continuous monitoring of external changes.</remarks>
static int verify_worker_cpu_affinity(int expected_logical_cpu)
{
    cpu_set_t effective_cpu_set;

    require_pthread_success(
        pthread_getaffinity_np(pthread_self(), sizeof(effective_cpu_set), &effective_cpu_set),
        "pthread_getaffinity_np");

    int observed_logical_cpu = sched_getcpu();

    if (observed_logical_cpu < 0) 
    {
        exit_with_error("sched_getcpu: %s", strerror(errno));
    }

    if (CPU_COUNT(&effective_cpu_set) != 1 ||
        !CPU_ISSET(expected_logical_cpu, &effective_cpu_set) ||
        observed_logical_cpu != expected_logical_cpu) 
    {
        exit_with_error(
            "Expected singleton affinity to CPU %d; observed CPU %d",
            expected_logical_cpu,
            observed_logical_cpu);
    }

    return observed_logical_cpu;
}

/// <summary>Blocks the calling worker between batches to reduce its runnable demand.</summary>
/// <param name="idle_microseconds">Requested idle duration; actual sleep may be longer.</param>
static void sleep_between_work_batches(uint64_t idle_microseconds)
{
    struct timespec remaining_sleep = 
    {
        .tv_sec = (time_t)(idle_microseconds / 1000000),
        .tv_nsec = (long)((idle_microseconds % 1000000) * 1000)
    };

    /* After interruption by a signal, nanosleep supplies the time still to sleep. */
    while (nanosleep(&remaining_sleep, &remaining_sleep) != 0)
    {
        if (errno != EINTR) 
        {
            exit_with_error("nanosleep: %s", strerror(errno));
        }
    }
}

/// <summary>Waits until all four workers and the main thread reach the same phase.</summary>
/// <param name="phase_barrier">The five-participant barrier used by this run.</param>
static void wait_for_benchmark_phase(pthread_barrier_t *phase_barrier)
{
    int barrier_status = pthread_barrier_wait(phase_barrier);

    /* One participant receives a special success value, not an error. */
    if (barrier_status != 0 && barrier_status != PTHREAD_BARRIER_SERIAL_THREAD)
    {
        exit_with_error("pthread_barrier_wait: %s", strerror(barrier_status));
    }
}

/// <summary>Terminates the run if a pthread operation reports an error.</summary>
/// <param name="error_code">The error code returned directly by the pthread operation.</param>
/// <param name="operation_name">Operation name to include in the diagnostic.</param>
/// <remarks>Pthread APIs return an error code rather than setting errno for this check.</remarks>
static void require_pthread_success(int error_code, const char *operation_name)
{
    if (error_code != 0) 
    {
        exit_with_error("%s: %s", operation_name, strerror(error_code));
    }
}

/// <summary>Reads the selected clock and converts its timestamp to nanoseconds.</summary>
/// <param name="clock_id">A monotonic wall-time clock or the current thread's CPU-time clock.</param>
/// <returns>The clock reading in nanoseconds.</returns>
/// <remarks>Only differences between readings of the same clock are meaningful here.</remarks>
static uint64_t read_clock_nanoseconds(clockid_t clock_id)
{
    struct timespec timestamp;

    if (clock_gettime(clock_id, &timestamp) != 0) 
    {
        exit_with_error("clock_gettime: %s", strerror(errno));
    }

    return (uint64_t)timestamp.tv_sec * UINT64_C(1000000000) +
           (uint64_t)timestamp.tv_nsec;
}

/// <summary>Reports an error to stderr and terminates the entire benchmark process.</summary>
/// <param name="error_format">A printf-style error format.</param>
/// <remarks>
/// Remaining arguments supply the format values. This function never returns;
/// a worker failure also ends the process so other threads cannot wait forever at a barrier.
/// </remarks>
static void exit_with_error(const char *error_format, ...)
{
    va_list format_arguments;

    va_start(format_arguments, error_format);
    vfprintf(stderr, error_format, format_arguments);
    va_end(format_arguments);

    fputc('\n', stderr);
    exit(EXIT_FAILURE);
}

/// <summary>Runs one experiment and writes validated JSON measurements to stdout or a text file.</summary>
/// <param name="argument_count">Number of arguments, including the executable name.</param>
/// <param name="arguments">Argument strings selecting CPUs, placement, layout, and workload size.</param>
/// <returns>EXIT_SUCCESS for a completed run or help; errors terminate with EXIT_FAILURE.</returns>
int main(int argument_count, char **arguments)
{
    const struct benchmark_options options = parse_benchmark_options(argument_count, arguments);
    size_t cache_line_bytes = validate_cpu_topology(&options);
    struct cpu_frequency_snapshot frequency_before[WORKER_COUNT];
    struct cpu_frequency_snapshot frequency_after[WORKER_COUNT];
    capture_cpu_frequency(&options, frequency_before);
    void *counter_storage = NULL;

    /* An aligned base makes the byte offsets below correspond to cache-line boundaries. */
    int allocation_error = posix_memalign(&counter_storage, cache_line_bytes, 4 * cache_line_bytes);

    if (allocation_error != 0) 
    {
        exit_with_error("posix_memalign: %s", strerror(allocation_error));
    }

    /* Arithmetic on unsigned char* advances in bytes, not in counter-sized elements.
       Shared: line 0 has T2/T3, line 1 is unused, lines 2/3 have T1/T4.
       Padded: T3 moves to line 1. Both layouts keep the same allocation size. */
    unsigned char *counter_bytes = counter_storage;

    const size_t counter_offsets[WORKER_COUNT] = 
    {
        2 * cache_line_bytes,
        0,
        options.separate_sharing_counters ? cache_line_bytes : sizeof(atomic_uint_fast64_t),
        3 * cache_line_bytes
    };

    /* Swap T1 and T3: T2/T3 share core A, and each core still has two workers. */
    const int baseline_cpu_slot_indices[WORKER_COUNT] = {0, 1, 2, 3};
    const int colocated_cpu_slot_indices[WORKER_COUNT] = {2, 1, 0, 3};
    const int *worker_cpu_slot_indices = options.colocate_sharing_threads
        ? colocated_cpu_slot_indices
        : baseline_cpu_slot_indices;

    pthread_barrier_t phase_barrier;

    require_pthread_success(
        pthread_barrier_init(&phase_barrier, NULL, WORKER_COUNT + 1),
        "pthread_barrier_init");

    pthread_t worker_threads[WORKER_COUNT];

    /* These records stay on main's stack until every pthread_join completes. */
    struct worker_state worker_states[WORKER_COUNT] = {0};
    uint64_t benchmark_start_nanoseconds = 0;

    for (int worker_index = 0; worker_index < WORKER_COUNT; ++worker_index)
    {
        worker_states[worker_index].options = &options;
        worker_states[worker_index].phase_barrier = &phase_barrier;
        worker_states[worker_index].benchmark_start_nanoseconds = &benchmark_start_nanoseconds;
        worker_states[worker_index].assigned_logical_cpu = options.logical_cpu_slots[worker_cpu_slot_indices[worker_index]];
        worker_states[worker_index].compute_checksum = (uint64_t)worker_index + 1;
        worker_states[worker_index].operation_counter = (atomic_uint_fast64_t *)(counter_bytes + counter_offsets[worker_index]);

        atomic_init(worker_states[worker_index].operation_counter, 0);

        /* A hidden locking implementation would change the experiment's contention. */
        if (!atomic_is_lock_free(worker_states[worker_index].operation_counter))
        {
            exit_with_error("This target does not provide lock-free counter operations");
        }

        require_pthread_success(
            pthread_create(
                &worker_threads[worker_index],
                NULL,
                run_pinned_worker,
                &worker_states[worker_index]),
            "pthread_create");
    }

    /* Five participants rendezvous three times: ready, start, finish.
       The start barrier also publishes the common timestamp before workers read it. */
    wait_for_benchmark_phase(&phase_barrier);
    benchmark_start_nanoseconds = read_clock_nanoseconds(CLOCK_MONOTONIC);
    wait_for_benchmark_phase(&phase_barrier);
    wait_for_benchmark_phase(&phase_barrier);

    uint64_t elapsed_nanoseconds = read_clock_nanoseconds(CLOCK_MONOTONIC) - benchmark_start_nanoseconds;
    uint64_t total_worker_cpu_nanoseconds = 0;

    for (int worker_index = 0; worker_index < WORKER_COUNT; ++worker_index)
    {
        /* Joining waits for thread exit and makes its result writes visible to main. */
        require_pthread_success(pthread_join(worker_threads[worker_index], NULL), "pthread_join");

        if (atomic_load_explicit(
                worker_states[worker_index].operation_counter,
                memory_order_relaxed) != options.increments_per_worker) {
            exit_with_error("Thread %d did not complete its expected increments", worker_index + 1);
        }

        total_worker_cpu_nanoseconds += worker_states[worker_index].consumed_cpu_nanoseconds;

        if (worker_states[worker_index].completion_nanoseconds == 0)
        {
            exit_with_error("Thread %d measurement interval was zero; increase --iterations", worker_index + 1);
        }
    }

    if (elapsed_nanoseconds == 0) 
    {
        exit_with_error("Measurement interval was zero; increase --iterations");
    }

    require_pthread_success(pthread_barrier_destroy(&phase_barrier), "pthread_barrier_destroy");
    capture_cpu_frequency(&options, frequency_after);

    double elapsed_seconds = (double)elapsed_nanoseconds / 1e9;

    FILE *results_stream = stdout;
    const char *results_destination = options.results_file_path != NULL
        ? options.results_file_path
        : "stdout";

    /* Open only after timing and validation; append mode preserves previous runs. */
    if (options.results_file_path != NULL)
    {
        results_stream = fopen(options.results_file_path, "a");

        if (results_stream == NULL)
        {
            exit_with_error("Cannot open results file %s: %s", results_destination, strerror(errno));
        }
    }

    /* C concatenates adjacent string literals. PRIu64 selects the portable uint64_t format.
       Summed worker CPU time / wall time is CPU equivalents, not physical-core utilization. */
    fprintf(
        results_stream,
        "{\"schema_version\":3,\"pid\":%ld,\"placement\":\"%s\",\"layout\":\"%s\","
        "\"cpu_slots\":[%d,%d,%d,%d],\"cache_line_bytes\":%zu,"
        "\"iterations_per_thread\":%" PRIu64 ",\"compute\":%" PRIu64 ","
        "\"target_increments_per_second_per_thread\":%" PRIu64 ","
        "\"nominal_work_seconds\":%.9f,"
        "\"batch\":%" PRIu64 ",\"idle_us\":%" PRIu64 ","
        "\"elapsed_seconds\":%.9f,\"increments_per_second\":%.3f,"
        "\"scheduled_cpu_equivalents\":%.6f,\"threads\":[",
        (long)getpid(),
        options.colocate_sharing_threads ? "colocated" : "baseline",
        options.separate_sharing_counters ? "padded" : "shared",
        options.logical_cpu_slots[0],
        options.logical_cpu_slots[1],
        options.logical_cpu_slots[2],
        options.logical_cpu_slots[3],
        cache_line_bytes,
        options.increments_per_worker,
        options.compute_steps_per_increment,
        options.target_increments_per_second,
        options.target_increments_per_second != 0
            ? (double)options.increments_per_worker / (double)options.target_increments_per_second
            : 0.0,
        options.increments_per_batch,
        options.idle_microseconds_per_batch,
        elapsed_seconds,
        (double)options.increments_per_worker * WORKER_COUNT / elapsed_seconds,
        (double)total_worker_cpu_nanoseconds / (double)elapsed_nanoseconds);

    for (int worker_index = 0; worker_index < WORKER_COUNT; ++worker_index) 
    {
        fprintf(
            results_stream,
            "%s{\"thread\":%d,\"tid\":%ld,\"cpu\":%d,"
            "\"start_cpu\":%d,\"end_cpu\":%d,"
            "\"counter_address\":\"%p\",\"counter_offset\":%zu,"
            "\"completion_seconds\":%.9f,\"cpu_seconds\":%.9f,"
            "\"increments_per_second\":%.3f,"
            "\"cpu_percent\":%.3f,\"checksum\":%" PRIu64 ","
            "\"voluntary_context_switches\":%ld,\"involuntary_context_switches\":%ld,"
            "\"minor_page_faults\":%ld,\"major_page_faults\":%ld,\"pmu\":",
            worker_index == 0 ? "" : ",",
            worker_index + 1,
            worker_states[worker_index].linux_thread_id,
            worker_states[worker_index].assigned_logical_cpu,
            worker_states[worker_index].observed_start_cpu,
            worker_states[worker_index].observed_end_cpu,
            (void *)worker_states[worker_index].operation_counter,
            counter_offsets[worker_index],
            (double)worker_states[worker_index].completion_nanoseconds / 1e9,
            (double)worker_states[worker_index].consumed_cpu_nanoseconds / 1e9,
            (double)options.increments_per_worker /
                ((double)worker_states[worker_index].completion_nanoseconds / 1e9),
            100.0 * (double)worker_states[worker_index].consumed_cpu_nanoseconds /
                (double)elapsed_nanoseconds,
            worker_states[worker_index].compute_checksum,
            worker_states[worker_index].voluntary_context_switches,
            worker_states[worker_index].involuntary_context_switches,
            worker_states[worker_index].minor_page_faults,
            worker_states[worker_index].major_page_faults);

        write_worker_pmu_results(results_stream, &worker_states[worker_index]);
        fputc('}', results_stream);
    }

    fputs("],\"metadata\":", results_stream);
    write_run_metadata(results_stream, &options, argument_count, arguments, frequency_before, frequency_after);
    fputs("}\n", results_stream);

    /* Buffered writes can fail at flush or close, not just at fprintf. */
    if (ferror(results_stream) || fflush(results_stream) != 0)
    {
        exit_with_error("Cannot write benchmark results to %s", results_destination);
    }

    if (options.results_file_path != NULL && fclose(results_stream) != 0)
    {
        exit_with_error("Cannot close results file %s: %s", results_destination, strerror(errno));
    }

    free(counter_storage);

    return EXIT_SUCCESS;
}
