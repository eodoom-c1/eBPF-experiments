#ifndef WORKLOAD_COMMON_H
#define WORKLOAD_COMMON_H

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define WORKER_COUNT 4

/// <summary>Reports an error and terminates the workload process.</summary>
static inline void exit_with_error(const char *error_format, ...)
{
    va_list arguments;
    va_start(arguments, error_format);
    vfprintf(stderr, error_format, arguments);
    va_end(arguments);
    fputc('\n', stderr);
    exit(EXIT_FAILURE);
}

/// <summary>Checks APIs that return a pthread error code rather than errno.</summary>
static inline void require_pthread_success(int error_code, const char *operation_name)
{
    if (error_code != 0)
    {
        exit_with_error("%s: %s", operation_name, strerror(error_code));
    }
}

/// <summary>Returns nanoseconds from the selected monotonic or thread CPU clock.</summary>
static inline uint64_t read_clock_nanoseconds(clockid_t clock_id)
{
    struct timespec timestamp;
    if (clock_gettime(clock_id, &timestamp) != 0)
    {
        exit_with_error("clock_gettime: %s", strerror(errno));
    }
    return (uint64_t)timestamp.tv_sec * UINT64_C(1000000000) + (uint64_t)timestamp.tv_nsec;
}

/// <summary>Parses a complete unsigned decimal value within inclusive bounds.</summary>
static inline uint64_t parse_bounded_unsigned_integer(
    const char *input_text, const char *value_name, uint64_t minimum_value, uint64_t maximum_value)
{
    char *suffix;
    errno = 0;
    unsigned long long value = strtoull(input_text, &suffix, 10);
    if (input_text[0] < '0' || input_text[0] > '9' || *suffix != '\0' ||
        errno == ERANGE || value < minimum_value || value > maximum_value)
    {
        exit_with_error("%s must be an integer from %" PRIu64 " to %" PRIu64,
            value_name, minimum_value, maximum_value);
    }
    return (uint64_t)value;
}

/// <summary>Parses exactly four CPU IDs in A0,A1,B0,B1 order.</summary>
static inline void parse_cpu_slots(const char *input_text, int slots[WORKER_COUNT])
{
    for (int index = 0; index < WORKER_COUNT; ++index)
    {
        char *suffix;
        errno = 0;
        unsigned long value = strtoul(input_text, &suffix, 10);
        char separator = index == WORKER_COUNT - 1 ? '\0' : ',';
        if (input_text[0] < '0' || input_text[0] > '9' || errno == ERANGE ||
            value >= CPU_SETSIZE || *suffix != separator)
        {
            exit_with_error("--cpus requires exactly A0,A1,B0,B1, each from 0 to %d", CPU_SETSIZE - 1);
        }
        slots[index] = (int)value;
        if (separator != '\0')
        {
            input_text = suffix + 1;
        }
    }
}

/// <summary>Reads a complete sysfs CPU attribute, removing the final newline.</summary>
static inline void read_cpu_sysfs_attribute(int cpu, const char *property, char *text, size_t capacity)
{
    char path[256];
    int length = snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/%s", cpu, property);
    if (length < 0 || (size_t)length >= sizeof(path))
    {
        exit_with_error("CPU property path is too long");
    }
    FILE *file = fopen(path, "r");
    if (file == NULL)
    {
        exit_with_error("%s: %s", path, strerror(errno));
    }
    if (fgets(text, (int)capacity, file) == NULL)
    {
        exit_with_error("Cannot read %s", path);
    }
    if (strchr(text, '\n') == NULL || ferror(file))
    {
        exit_with_error("Incomplete CPU property: %s", path);
    }
    if (fclose(file) != 0)
    {
        exit_with_error("Cannot close %s: %s", path, strerror(errno));
    }
    text[strcspn(text, "\n")] = '\0';
}

/// <summary>Checks allowed CPUs, two distinct physical cores, and cache-line alignment.</summary>
static inline size_t validate_cpu_slots(const int slots[WORKER_COUNT])
{
    cpu_set_t allowed;
    if (sched_getaffinity(0, sizeof(allowed), &allowed) != 0)
    {
        exit_with_error("sched_getaffinity (CPU_SETSIZE=%d): %s", CPU_SETSIZE, strerror(errno));
    }
    char siblings[WORKER_COUNT][4096];
    size_t common_line_bytes = 0;
    for (int index = 0; index < WORKER_COUNT; ++index)
    {
        int cpu = slots[index];
        if (!CPU_ISSET(cpu, &allowed))
        {
            exit_with_error("CPU %d is outside the process's allowed CPU set", cpu);
        }
        read_cpu_sysfs_attribute(cpu, "topology/thread_siblings_list", siblings[index], sizeof(siblings[index]));
        char line_text[64];
        read_cpu_sysfs_attribute(cpu, "cache/index0/coherency_line_size", line_text, sizeof(line_text));
        size_t line_bytes = (size_t)parse_bounded_unsigned_integer(
            line_text, "cache-line size", sizeof(atomic_uint_fast64_t) * 2, 4096);
        if ((line_bytes & (line_bytes - 1)) != 0 || line_bytes < sizeof(void *))
        {
            exit_with_error("Unsupported cache-line alignment: %zu", line_bytes);
        }
        if (index != 0 && line_bytes != common_line_bytes)
        {
            exit_with_error("Selected CPUs have different cache-line sizes");
        }
        common_line_bytes = line_bytes;
    }
    if (strcmp(siblings[0], siblings[1]) != 0 || strcmp(siblings[2], siblings[3]) != 0 ||
        strcmp(siblings[0], siblings[2]) == 0)
    {
        exit_with_error("--cpus must describe two logical-CPU pairs on distinct physical cores");
    }
    return common_line_bytes;
}

/// <summary>Checks singleton thread affinity and the CPU observed at an endpoint.</summary>
static inline int verify_worker_cpu_affinity(int expected_cpu)
{
    cpu_set_t effective;
    require_pthread_success(
        pthread_getaffinity_np(pthread_self(), sizeof(effective), &effective), "pthread_getaffinity_np");
    int observed = sched_getcpu();
    if (observed < 0)
    {
        exit_with_error("sched_getcpu: %s", strerror(errno));
    }
    if (CPU_COUNT(&effective) != 1 || !CPU_ISSET(expected_cpu, &effective) || observed != expected_cpu)
    {
        exit_with_error("Expected singleton affinity to CPU %d; observed CPU %d", expected_cpu, observed);
    }
    return observed;
}

#endif
