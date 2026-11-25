// SPDX-License-Identifier: (MIT OR GPL-2.0-only)
#include <linux/bpf.h>
#include <linux/errno.h>
#include <bpf/bpf_helpers.h>

#include "syscall_counts.h"

// Keys are shared across CPUs, but each key has a separate counter value on
// every possible CPU. Userspace must sum those values to obtain process totals.
// __uint supplies numeric metadata (map kind/capacity); __type describes the
// key/value C types. These macros encode a map definition in the object's BTF,
// not runtime counters inside this C struct. libbpf creates the map when loading.
struct
{
    __uint(type, BPF_MAP_TYPE_PERCPU_HASH);
    __uint(max_entries, MAX_TRACKED_PROCESSES);
    __type(key, __u32);
    __type(value, struct syscall_counts);
} process_counts SEC(".maps");

// This fixed-size map remains available even when the process hash is full.
struct
{
    __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, __u64);
} missed_attempts SEC(".maps");

enum syscall_kind
{
    EXECVE_ATTEMPT,
    OPENAT_ATTEMPT
};

/// <summary>
/// Records an attempt that could not be added to the process counters.
/// </summary>
/// <remarks>
/// The array has one entry per CPU, so this does not need a new hash allocation.
/// </remarks>
static __always_inline void record_missed_attempt(void)
{
    // An array map uses an integer index, not a TGID. Index 0 is its only entry.
    // The kernel helper returns this CPU's live 64-bit counter, or NULL.
    __u32 key = 0;
    __u64 *missed = bpf_map_lookup_elem(&missed_attempts, &key);

    if (missed == NULL)
    {
        bpf_printk("p2_counts missed-attempt map lookup failed");
        return;
    }

    (*missed)++;
}

/// <summary>
/// Increments the current process's counter for one selected syscall attempt.
/// </summary>
/// <param name="kind">The counter selected by the attached tracepoint wrapper.</param>
/// <returns>Zero; this tracing program does not control syscall execution.</returns>
static __always_inline int count_attempt(enum syscall_kind kind)
{
    // The upper half is the TGID, so all threads in a process share one key.
    __u32 tgid = bpf_get_current_pid_tgid() >> 32;

    // Find key tgid in process_counts. &tgid supplies the bytes of the key.
    // Inside BPF, this helper returns this CPU's live value pointer, or NULL if
    // the key is absent. It does not create the key and does not copy the value.
    // Changing fields through the returned pointer updates the kernel map.
    struct syscall_counts *counts = bpf_map_lookup_elem(&process_counts, &tgid);

    if (counts == NULL)
    {
        struct syscall_counts initial = {0};

        // Ask the kernel to insert tgid and COPY the zero counters from initial;
        // the map does not keep a pointer to this temporary stack variable.
        // BPF_NOEXIST forbids replacing an existing key. The helper returns 0
        // on success or a negative errno value on failure (no userspace errno).
        // Another CPU may insert this TGID first, giving us -EEXIST; that is
        // expected, while allocation/capacity failures mean we missed this event.
        long error = bpf_map_update_elem(&process_counts, &tgid, &initial, BPF_NOEXIST);
        if (error != 0 && error != -EEXIST)
        {
            record_missed_attempt();
            return 0;
        }

        // Lookup again after insertion, including the expected EEXIST race.
        counts = bpf_map_lookup_elem(&process_counts, &tgid);
        if (counts == NULL)
        {
            record_missed_attempt();
            return 0;
        }
    }

    // These non-sleepable syscall hooks update this CPU's value. Other CPUs
    // update their own values rather than contending for this counter pair.
    if (kind == EXECVE_ATTEMPT)
    {
        counts->execve++;
    }
    else
    {
        counts->openat++;
    }

    return 0;
}

/// <summary>Counts execve attempts, including attempts that later fail.</summary>
/// <param name="ctx">Unused kernel-supplied syscall tracepoint arguments.</param>
/// <returns>Zero; the syscall continues independently of this observer.</returns>
SEC("tracepoint/syscalls/sys_enter_execve")
int count_execve(void *ctx)
{
    (void)ctx;
    return count_attempt(EXECVE_ATTEMPT);
}

/// <summary>Counts openat attempts, including attempts that later fail.</summary>
/// <param name="ctx">Unused kernel-supplied syscall tracepoint arguments.</param>
/// <returns>Zero; the syscall continues independently of this observer.</returns>
SEC("tracepoint/syscalls/sys_enter_openat")
int count_openat(void *ctx)
{
    (void)ctx;
    return count_attempt(OPENAT_ATTEMPT);
}

// GPL compatibility permits the diagnostic printing helper used on an error.
char LICENSE[] SEC("license") = "Dual MIT/GPL";
