// SPDX-License-Identifier: MIT
#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include "syscall_counts.h"
#include "syscall_counts.skel.h"

struct process_row
{
    __u32 tgid;
    struct syscall_counts counts;
    uint64_t total;
};

static volatile sig_atomic_t stop_requested;

/// <summary>Requests shutdown without doing unsafe work inside a signal handler.</summary>
/// <param name="signal_number">SIGINT or SIGTERM delivered by the operating system.</param>
static void request_stop(int signal_number)
{
    (void)signal_number;
    stop_requested = 1;
}

/// <summary>Orders rows by descending total attempts, then ascending TGID.</summary>
/// <param name="left">The first process_row supplied by qsort.</param>
/// <param name="right">The second process_row supplied by qsort.</param>
/// <returns>Negative, zero, or positive according to the requested ordering.</returns>
static int compare_rows(const void *left, const void *right)
{
    const struct process_row *a = left;
    const struct process_row *b = right;

    // Comparisons avoid narrowing a potentially large counter difference.
    if (a->total != b->total)
    {
        return a->total > b->total ? -1 : 1;
    }

    return (a->tgid > b->tgid) - (a->tgid < b->tgid);
}

/// <summary>
/// Reads the process map, sums every possible CPU's values, and prints the ten
/// busiest TGIDs along with cumulative missed-attempt accounting.
/// </summary>
/// <param name="process_fd">Descriptor for the per-CPU process hash map.</param>
/// <param name="missed_fd">Descriptor for the one-entry per-CPU missed counter.</param>
/// <param name="cpu_count">Number of possible CPUs, including currently offline CPUs.</param>
/// <param name="cpu_values">Buffer for one process's complete per-CPU value array.</param>
/// <param name="missed_values">Buffer for the missed counter's per-CPU value array.</param>
/// <param name="rows">Buffer holding MAX_TRACKED_PROCESSES aggregated rows.</param>
/// <returns>Zero on success or -1 after reporting a map-read failure.</returns>
/// <remarks>
/// Map traversal takes two separate operations: get_next_key finds a TGID;
/// lookup_elem then reads the counters stored under that TGID. Neither operation
/// freezes the map, so this is an approximate snapshot of live counters.
/// </remarks>
static int print_snapshot(int process_fd, int missed_fd, int cpu_count,
                          struct syscall_counts *cpu_values,
                          __u64 *missed_values, struct process_row *rows)
{
    __u32 key;
    __u32 next_key;
    __u32 missed_key = 0;
    size_t row_count = 0;
    uint64_t missed_total = 0;

    // Ask the kernel for the first key in the map identified by process_fd.
    // Argument 2 is the previous key's address; NULL means "start iteration".
    // Argument 3 is an OUTPUT address: the kernel writes a TGID into next_key.
    // Return 0 means a key was copied; failure sets errno. An empty map reports
    // ENOENT. This call reads only a key, not its counters, and changes no entries.
    int result = bpf_map_get_next_key(process_fd, NULL, &next_key);

    // Only use next_key when the previous call succeeded. Hash-map traversal
    // follows the map's internal order, not ascending TGID or syscall count.
    while (result == 0)
    {
        key = next_key;
        if (row_count == MAX_TRACKED_PROCESSES)
        {
            fprintf(stderr, "Process map exceeded the shared row-buffer capacity.\n");
            return -1;
        }

        // Read the VALUE associated with key: process_fd identifies the map,
        // &key supplies the TGID to find, and cpu_values receives the copied data.
        // Return 0 means success; failure sets errno. Unlike the BPF helper,
        // this userspace function returns a status, not a map-value pointer.
        // For this per-CPU map, one lookup copies [CPU0 value][CPU1 value]...
        // into cpu_values, including slots belonging to offline possible CPUs.
        if (bpf_map_lookup_elem(process_fd, &key, cpu_values) != 0)
        {
            perror("Failed to read a process counter");
            return -1;
        }

        // The compound literal sets this row's TGID and zeroes its other fields.
        // This prevents adding a new snapshot onto totals left by the last one.
        rows[row_count] = (struct process_row){.tgid = key};
        // A process can make syscalls on several CPUs. Add their independent
        // values: for example, 5 openat calls on CPU0 plus 3 on CPU1 means 8.
        for (int cpu = 0; cpu < cpu_count; cpu++)
        {
            rows[row_count].counts.execve += cpu_values[cpu].execve;
            rows[row_count].counts.openat += cpu_values[cpu].openat;
        }

        rows[row_count].total = rows[row_count].counts.execve +
                                rows[row_count].counts.openat;
        row_count++;

        // Continue the stateless traversal: &key is the TGID just processed;
        // &next_key receives the following TGID. No iterator object is kept
        // inside this API. At the end it fails with errno == ENOENT.
        // If key no longer existed, traversal would restart at the first key;
        // our BPF programs retain keys until shutdown, avoiding that situation.
        result = bpf_map_get_next_key(process_fd, &key, &next_key);
    }

    // ENOENT means the iteration reached its end, not that collection failed.
    if (errno != ENOENT)
    {
        perror("Failed to iterate process counters");
        return -1;
    }

    // The missed-attempt ARRAY has one logical index, 0, with a value per CPU.
    // Look up that index and copy all CPU slots into missed_values. This is a
    // different map and value type from the TGID hash, but uses the same API.
    if (bpf_map_lookup_elem(missed_fd, &missed_key, missed_values) != 0)
    {
        perror("Failed to read missed-attempt counters");
        return -1;
    }

    for (int cpu = 0; cpu < cpu_count; cpu++)
    {
        missed_total += missed_values[cpu];
    }

    // Map traversal did not rank processes. qsort rearranges our userspace rows
    // using compare_rows, so displaying the first ten yields the largest totals.
    qsort(rows, row_count, sizeof(*rows), compare_rows);
    printf("\nCumulative attempts: %zu tracked TGIDs, %" PRIu64 " missed\n",
           row_count, missed_total);
    printf("%-10s %16s %16s %16s\n", "TGID", "EXECVE", "OPENAT", "TOTAL");
    for (size_t index = 0; index < row_count && index < 10; index++)
    {
        printf("%-10u %16" PRIu64 " %16" PRIu64 " %16" PRIu64 "\n",
               rows[index].tgid, (uint64_t)rows[index].counts.execve,
               (uint64_t)rows[index].counts.openat, rows[index].total);
    }

    fflush(stdout);
    return 0;
}

/// <summary>
/// Loads and attaches the two syscall observers, displays cumulative snapshots
/// once per second, and releases all attachments and buffers on exit.
/// </summary>
/// <returns>EXIT_SUCCESS on normal shutdown or EXIT_FAILURE after reporting an error.</returns>
int main(void)
{
    struct syscall_counts_bpf *skeleton = NULL;
    struct syscall_counts *cpu_values = NULL;
    __u64 *missed_values = NULL;
    struct process_row *rows = NULL;
    struct sigaction action = {0};
    int status = EXIT_FAILURE;
    int cpu_count;
    int error;

    // sigaction replaces the default termination behavior for these signals.
    // The handler only sets a flag; this loop then exits through normal cleanup.
    action.sa_handler = request_stop;
    if (sigemptyset(&action.sa_mask) != 0 ||
        sigaction(SIGINT, &action, NULL) != 0 ||
        sigaction(SIGTERM, &action, NULL) != 0)
    {
        perror("Failed to install shutdown signal handlers");
        return EXIT_FAILURE;
    }

    // libbpf reads Linux's possible-CPU set and returns its count, or an error.
    // Online CPU count is insufficient: the kernel's map layout reserves a
    // slot for each possible CPU, even when some CPUs are currently offline.
    cpu_count = libbpf_num_possible_cpus();
    if (cpu_count <= 0)
    {
        fprintf(stderr, "Failed to determine possible CPU count (%d).\n", cpu_count);
        return EXIT_FAILURE;
    }

    // These buffers live in userspace, not in the BPF maps. A map lookup will
    // copy one value per possible CPU into each CPU buffer. rows holds the
    // aggregated process data that we can sort without modifying kernel maps.
    cpu_values = calloc((size_t)cpu_count, sizeof(*cpu_values));
    missed_values = calloc((size_t)cpu_count, sizeof(*missed_values));
    rows = calloc(MAX_TRACKED_PROCESSES, sizeof(*rows));
    if (cpu_values == NULL || missed_values == NULL || rows == NULL)
    {
        fprintf(stderr, "Failed to allocate map-reading buffers.\n");
        goto cleanup;
    }

    // This generated skeleton function parses its embedded BPF ELF object and
    // exposes libbpf map/program objects. It does not load or attach them yet.
    skeleton = syscall_counts_bpf__open();
    if (skeleton == NULL)
    {
        fprintf(stderr, "Failed to open the BPF object; see libbpf diagnostics.\n");
        goto cleanup;
    }

    // Loading creates the maps and asks the verifier to accept both programs.
    error = syscall_counts_bpf__load(skeleton);
    if (error != 0)
    {
        fprintf(stderr, "Failed to load syscall observers: %s (%d).\n",
                strerror(-error), error);
        goto cleanup;
    }

    // Both SEC annotations are attached. A partial failure is cleaned up by
    // destroying the skeleton, including any attachment already created.
    error = syscall_counts_bpf__attach(skeleton);
    if (error != 0)
    {
        fprintf(stderr, "Failed to attach syscall observers: %s (%d).\n",
                strerror(-error), error);
        goto cleanup;
    }

    puts("Counting execve/openat attempts. Press Ctrl+C to detach.");
    puts("Totals are cumulative since attachment; snapshots are not atomic.");
    while (!stop_requested)
    {
        // bpf_map__fd obtains the Linux file descriptor of a loaded map object.
        // Userspace map APIs pass this integer to the bpf syscall so the kernel
        // knows which map to access; a libbpf map-object pointer is not that FD.
        if (print_snapshot(bpf_map__fd(skeleton->maps.process_counts),
                           bpf_map__fd(skeleton->maps.missed_attempts),
                           cpu_count, cpu_values, missed_values, rows) != 0)
        {
            goto cleanup;
        }

        sleep(1);
    }

    status = EXIT_SUCCESS;
    puts("Detaching syscall observers.");

cleanup:
    // No maps or links are pinned. Closing their last references removes them.
    syscall_counts_bpf__destroy(skeleton);
    free(rows);
    free(missed_values);
    free(cpu_values);
    return status;
}
