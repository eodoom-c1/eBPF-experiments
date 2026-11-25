// SPDX-License-Identifier: MIT
#ifndef SYSCALL_COUNTS_H
#define SYSCALL_COUNTS_H

#include <linux/types.h>

#define MAX_TRACKED_PROCESSES 4096

// Both programs share this map-value layout: each CPU stores its own instance
// for a TGID. A userspace lookup receives an array of these instances.
// The kernel rounds each CPU slot's size up to a multiple of eight bytes; our
// two 64-bit fields already occupy 16 bytes, so sizeof(struct syscall_counts)
// is also the slot spacing used by cpu_values[cpu].
struct syscall_counts
{
    __u64 execve;
    __u64 openat;
};

#endif
