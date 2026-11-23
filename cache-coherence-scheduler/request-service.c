#define _GNU_SOURCE

#include <arpa/inet.h>
#include <getopt.h>
#include <netinet/tcp.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "workload-common.h"

#define KEY_COUNT 65536
#define VALUE_BYTES 64
#define REQUEST_BYTES 16
#define RESPONSE_BYTES 80

/// <summary>Immutable configuration for the four-stream lookup service.</summary>
struct service_options {
    /// <summary>CPU slots in A0,A1,B0,B1 order.</summary>
    int cpu_slots[WORKER_COUNT];
    /// <summary>Whether to schedule T2/T3 together.</summary>
    bool colocated;
    /// <summary>Whether to separate T2/T3 statistics onto different lines.</summary>
    bool padded;
    /// <summary>First of four consecutive loopback TCP ports.</summary>
    int base_port;
};

/// <summary>One worker's inputs and endpoint measurements.</summary>
struct service_worker {
    /// <summary>Listening socket owned by this worker.</summary>
    int listener;
    /// <summary>Requested logical CPU.</summary>
    int cpu;
    /// <summary>Ready rendezvous shared with main.</summary>
    pthread_barrier_t *ready;
    /// <summary>Read-only key-value table.</summary>
    const unsigned char *values;
    /// <summary>This worker's completed-request statistics counter.</summary>
    atomic_uint_fast64_t *completed;
    /// <summary>CPU observed after affinity setup.</summary>
    int start_cpu;
    /// <summary>CPU observed after the connection closes.</summary>
    int end_cpu;
    /// <summary>Linux thread identifier.</summary>
    long tid;
    /// <summary>Scheduled user and kernel CPU nanoseconds during serving.</summary>
    uint64_t cpu_nanoseconds;
    /// <summary>Monotonic connection lifetime in nanoseconds.</summary>
    uint64_t elapsed_nanoseconds;
    /// <summary>Explicit peer-disconnect error, or zero for clean EOF.</summary>
    int connection_error;
};

/// <summary>Parses placement and loopback listener configuration.</summary>
static struct service_options parse_service_options(int argc, char **argv)
{
    struct service_options options = {.base_port = 19000};
    bool have_cpus = false;
    const struct option arguments[] = {
        {"cpus", required_argument, NULL, 'c'},
        {"placement", required_argument, NULL, 'p'},
        {"layout", required_argument, NULL, 'l'},
        {"base-port", required_argument, NULL, 'b'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0}
    };
    int argument;
    while ((argument = getopt_long(argc, argv, "", arguments, NULL)) != -1)
    {
        switch (argument)
        {
        case 'c':
            parse_cpu_slots(optarg, options.cpu_slots);
            have_cpus = true;
            break;
        case 'p':
            if (strcmp(optarg, "baseline") != 0 && strcmp(optarg, "colocated") != 0)
            {
                exit_with_error("--placement must be baseline or colocated; received '%s'", optarg);
            }
            options.colocated = strcmp(optarg, "colocated") == 0;
            break;
        case 'l':
            if (strcmp(optarg, "shared") != 0 && strcmp(optarg, "padded") != 0)
            {
                exit_with_error("--layout must be shared or padded; received '%s'", optarg);
            }
            options.padded = strcmp(optarg, "padded") == 0;
            break;
        case 'b':
            options.base_port = (int)parse_bounded_unsigned_integer(optarg, "--base-port", 1024, 65532);
            break;
        case 'h':
            puts("Usage: request-service --cpus A0,A1,B0,B1 [--placement baseline|colocated]"
                 " [--layout shared|padded] [--base-port 19000]\n"
                 "Four loopback ports map to T1-T4. Each accepts one connection."
                 " A request is a network-order uint64 request ID and uint64 key."
                 " Reply: uint64 ID, uint32 status, uint32 length, 64 value bytes.");
            exit(EXIT_SUCCESS);
        default:
            exit(EXIT_FAILURE);
        }
    }
    if (!have_cpus || optind != argc)
    {
        exit_with_error("Supply --cpus and no positional arguments");
    }
    return options;
}

/// <summary>Decodes a network-order 64-bit request field.</summary>
static uint64_t decode_u64(const unsigned char *bytes)
{
    uint64_t value = 0;
    for (int index = 0; index < 8; ++index)
    {
        value = (value << 8) | bytes[index];
    }
    return value;
}

/// <summary>Encodes a network-order 64-bit value field.</summary>
static void encode_u64(unsigned char *bytes, uint64_t value)
{
    for (int index = 7; index >= 0; --index)
    {
        bytes[index] = (unsigned char)value;
        value >>= 8;
    }
}

/// <summary>Builds deterministic immutable values outside the serving interval.</summary>
static void initialize_values(unsigned char *values)
{
    for (uint64_t key = 0; key < KEY_COUNT; ++key)
    {
        unsigned char *value = values + key * VALUE_BYTES;
        for (int word = 0; word < VALUE_BYTES / 8; ++word)
        {
            encode_u64(value + word * 8, key ^ (UINT64_C(0x9e3779b97f4a7c15) * (uint64_t)word));
        }
    }
}

/// <summary>Creates a single-connection listener bound only to IPv4 loopback.</summary>
static int open_listener(int port)
{
    int listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listener < 0)
    {
        exit_with_error("socket: %s", strerror(errno));
    }
    int enabled = 1;
    if (setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) != 0)
    {
        exit_with_error("SO_REUSEADDR: %s", strerror(errno));
    }
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons((uint16_t)port),
        .sin_addr = {.s_addr = htonl(INADDR_LOOPBACK)}
    };
    if (bind(listener, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(listener, 1) != 0)
    {
        exit_with_error("Listen on 127.0.0.1:%d: %s", port, strerror(errno));
    }
    return listener;
}

/// <summary>Reads one complete request, distinguishing clean EOF and truncated input.</summary>
static int read_request(int connection, unsigned char request[REQUEST_BYTES], int *connection_error)
{
    size_t received = 0;
    while (received < REQUEST_BYTES)
    {
        ssize_t count = recv(connection, request + received, REQUEST_BYTES - received, 0);
        if (count == 0)
        {
            if (received != 0)
            {
                *connection_error = EPROTO;
            }
            return 0;
        }
        if (count < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            if (errno == ECONNRESET)
            {
                *connection_error = errno;
                return 0;
            }
            exit_with_error("recv: %s", strerror(errno));
        }
        received += (size_t)count;
    }
    return 1;
}

/// <summary>Sends a complete response, reporting peer failure instead of hiding lost work.</summary>
static bool send_response(int connection, const unsigned char response[RESPONSE_BYTES], int *connection_error)
{
    size_t sent = 0;
    while (sent < RESPONSE_BYTES)
    {
        ssize_t count = send(connection, response + sent, RESPONSE_BYTES - sent, MSG_NOSIGNAL);
        if (count < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            if (errno == EPIPE || errno == ECONNRESET)
            {
                *connection_error = errno;
                return false;
            }
            exit_with_error("send: %s", strerror(errno));
        }
        if (count == 0)
        {
            exit_with_error("send returned zero before completing a response");
        }
        sent += (size_t)count;
    }
    return true;
}

/// <summary>Pins one stream worker and serves real lookup requests until EOF.</summary>
static void *serve_worker(void *argument)
{
    struct service_worker *worker = argument;
    cpu_set_t cpus;
    CPU_ZERO(&cpus);
    CPU_SET(worker->cpu, &cpus);
    require_pthread_success(
        pthread_setaffinity_np(pthread_self(), sizeof(cpus), &cpus), "pthread_setaffinity_np");
    worker->start_cpu = verify_worker_cpu_affinity(worker->cpu);
    worker->tid = syscall(SYS_gettid);
    if (worker->tid < 0)
    {
        exit_with_error("gettid: %s", strerror(errno));
    }
    int barrier = pthread_barrier_wait(worker->ready);
    if (barrier != 0 && barrier != PTHREAD_BARRIER_SERIAL_THREAD)
    {
        exit_with_error("Ready barrier: %s", strerror(barrier));
    }
    int connection;
    do
    {
        connection = accept4(worker->listener, NULL, NULL, SOCK_CLOEXEC);
    }
    while (connection < 0 && errno == EINTR);
    if (connection < 0)
    {
        exit_with_error("accept: %s", strerror(errno));
    }
    int enabled = 1;
    if (setsockopt(connection, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled)) != 0)
    {
        exit_with_error("TCP_NODELAY: %s", strerror(errno));
    }
    uint64_t start_cpu = read_clock_nanoseconds(CLOCK_THREAD_CPUTIME_ID);
    uint64_t start_wall = read_clock_nanoseconds(CLOCK_MONOTONIC);
    unsigned char request[REQUEST_BYTES];
    while (read_request(connection, request, &worker->connection_error))
    {
        uint64_t key = decode_u64(request + 8);
        unsigned char response[RESPONSE_BYTES] = {0};
        memcpy(response, request, 8);
        response[15] = VALUE_BYTES;
        if (key < KEY_COUNT)
        {
            memcpy(response + 16, worker->values + key * VALUE_BYTES, VALUE_BYTES);
        }
        else
        {
            response[11] = 1;
        }
        if (!send_response(connection, response, &worker->connection_error))
        {
            break;
        }
        atomic_fetch_add_explicit(worker->completed, 1, memory_order_relaxed);
    }
    worker->cpu_nanoseconds = read_clock_nanoseconds(CLOCK_THREAD_CPUTIME_ID) - start_cpu;
    worker->elapsed_nanoseconds = read_clock_nanoseconds(CLOCK_MONOTONIC) - start_wall;
    worker->end_cpu = verify_worker_cpu_affinity(worker->cpu);
    if (close(connection) != 0 || close(worker->listener) != 0)
    {
        exit_with_error("close connection: %s", strerror(errno));
    }
    return NULL;
}

/// <summary>Runs four independent loopback lookup streams and emits readiness and final JSON.</summary>
int main(int argc, char **argv)
{
    const struct service_options options = parse_service_options(argc, argv);
    size_t line_bytes = validate_cpu_slots(options.cpu_slots);
    unsigned char *values = malloc(KEY_COUNT * VALUE_BYTES);
    if (values == NULL)
    {
        exit_with_error("Cannot allocate key-value table");
    }
    initialize_values(values);
    void *statistics = NULL;
    int allocation = posix_memalign(&statistics, line_bytes, 4 * line_bytes);
    if (allocation != 0)
    {
        exit_with_error("Statistics allocation: %s", strerror(allocation));
    }
    const size_t offsets[WORKER_COUNT] = {
        2 * line_bytes, 0, options.padded ? line_bytes : sizeof(atomic_uint_fast64_t), 3 * line_bytes
    };
    const int baseline[WORKER_COUNT] = {0, 1, 2, 3};
    const int colocated[WORKER_COUNT] = {2, 1, 0, 3};
    const int *slots = options.colocated ? colocated : baseline;
    pthread_barrier_t ready;
    require_pthread_success(pthread_barrier_init(&ready, NULL, WORKER_COUNT + 1), "pthread_barrier_init");
    struct service_worker workers[WORKER_COUNT] = {0};
    pthread_t threads[WORKER_COUNT];
    for (int index = 0; index < WORKER_COUNT; ++index)
    {
        workers[index].listener = open_listener(options.base_port + index);
        workers[index].cpu = options.cpu_slots[slots[index]];
        workers[index].ready = &ready;
        workers[index].values = values;
        workers[index].completed = (atomic_uint_fast64_t *)((unsigned char *)statistics + offsets[index]);
        atomic_init(workers[index].completed, 0);
        if (!atomic_is_lock_free(workers[index].completed))
        {
            exit_with_error("Statistics counters must be lock-free");
        }
        require_pthread_success(pthread_create(&threads[index], NULL, serve_worker, &workers[index]), "pthread_create");
    }
    int barrier = pthread_barrier_wait(&ready);
    if (barrier != 0 && barrier != PTHREAD_BARRIER_SERIAL_THREAD)
    {
        exit_with_error("Ready barrier: %s", strerror(barrier));
    }
    printf("{\"type\":\"ready\",\"schema_version\":1,\"pid\":%ld,\"cache_line_bytes\":%zu,"
           "\"key_count\":%d,\"value_bytes\":%d,\"base_port\":%d}\n",
        (long)getpid(), line_bytes, KEY_COUNT, VALUE_BYTES, options.base_port);
    if (fflush(stdout) != 0)
    {
        exit_with_error("Cannot write readiness record");
    }
    for (int index = 0; index < WORKER_COUNT; ++index)
    {
        require_pthread_success(pthread_join(threads[index], NULL), "pthread_join");
    }
    require_pthread_success(pthread_barrier_destroy(&ready), "pthread_barrier_destroy");
    printf("{\"type\":\"result\",\"schema_version\":1,\"placement\":\"%s\",\"layout\":\"%s\","
           "\"build_timestamp\":\"%s %s\","
           "\"cpu_slots\":[%d,%d,%d,%d],\"cache_line_bytes\":%zu,\"threads\":[",
        options.colocated ? "colocated" : "baseline", options.padded ? "padded" : "shared",
        __DATE__, __TIME__,
        options.cpu_slots[0], options.cpu_slots[1], options.cpu_slots[2], options.cpu_slots[3], line_bytes);
    for (int index = 0; index < WORKER_COUNT; ++index)
    {
        printf("%s{\"thread\":%d,\"tid\":%ld,\"cpu\":%d,\"start_cpu\":%d,\"end_cpu\":%d,"
               "\"counter_offset\":%zu,\"completed_requests\":%" PRIuFAST64 ","
               "\"cpu_seconds\":%.9f,\"connection_seconds\":%.9f,\"connection_error\":%d}",
            index == 0 ? "" : ",", index + 1, workers[index].tid, workers[index].cpu,
            workers[index].start_cpu, workers[index].end_cpu, offsets[index],
            atomic_load_explicit(workers[index].completed, memory_order_relaxed),
            (double)workers[index].cpu_nanoseconds / 1e9, (double)workers[index].elapsed_nanoseconds / 1e9,
            workers[index].connection_error);
    }
    puts("]}");
    if (ferror(stdout) || fflush(stdout) != 0)
    {
        exit_with_error("Cannot write service results");
    }
    free(statistics);
    free(values);
    return EXIT_SUCCESS;
}
