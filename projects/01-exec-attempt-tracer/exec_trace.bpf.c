// SPDX-License-Identifier: (MIT OR GPL-2.0-only)
#include <linux/bpf.h>
#include <bpf/bpf_helpers.h>

/// <summary>
/// Emits the calling task's identity when it enters the execve syscall.
/// </summary>
/// <param name="ctx">
/// Tracepoint arguments supplied by the kernel.
/// </param>
/// <returns>
/// Zero. This tracepoint program observes the syscall and cannot allow or deny it.
/// </returns>
SEC("tracepoint/syscalls/sys_enter_execve")
int trace_execve(void *ctx)
{
    (void)ctx;

    // This helper packs the process TGID into the upper 32 bits and the thread
    // TID into the lower 32 bits. Both identify the task triggering the hook.
    __u64 identity = bpf_get_current_pid_tgid();
    __u32 tgid = identity >> 32;
    __u32 tid = (__u32)identity;
    char comm[16] = {0};

    // The helper safely copies the current task name into our stack buffer.
    // At syscall entry this is the OLD name, not the executable being requested.
    if (bpf_get_current_comm(comm, sizeof(comm)) != 0) 
    {
        bpf_printk("p1_exec tgid=%u tid=%u comm unavailable", tgid, tid);
        return 0;
    }

    // This helper writes to the kernel's shared diagnostic trace buffer.
    // It does not print directly to the userspace loader's terminal.
    bpf_printk("p1_exec tgid=%u tid=%u comm=%s", tgid, tid, comm);
    return 0;
}

// The kernel requires a GPL-compatible declaration for bpf_printk's helper.
// Dual MIT/GPL retains the MIT option while permitting that helper.
char LICENSE[] SEC("license") = "Dual MIT/GPL";
