// SPDX-License-Identifier: MIT
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <bpf/libbpf.h>
#include "exec_trace.skel.h"

static volatile sig_atomic_t stop_requested;

/// <summary>
/// Requests shutdown when the loader receives SIGINT or SIGTERM.
/// </summary>
/// <param name="signal_number">The signal delivered by the operating system.</param>
/// <remarks>
/// A signal handler must not perform ordinary logging or libbpf cleanup.
/// Updating a sig_atomic_t flag lets the main function do that work safely.
/// </remarks>
static void request_stop(int signal_number)
{
    (void)signal_number;
    stop_requested = 1;
}

/// <summary>
/// Loads the embedded BPF object, attaches its execve tracepoint program, and
/// keeps the attachment alive until shutdown is requested.
/// </summary>
/// <returns>
/// EXIT_SUCCESS after normal shutdown, or EXIT_FAILURE if signal setup,
/// BPF object opening, verifier loading, or tracepoint attachment fails.
/// </returns>
int main(void)
{
    struct exec_trace_bpf *skeleton = NULL;
    struct sigaction action = {0};
    int status = EXIT_FAILURE;
    int error;

    action.sa_handler = request_stop;
    if (sigemptyset(&action.sa_mask) != 0 ||
        sigaction(SIGINT, &action, NULL) != 0 ||
        sigaction(SIGTERM, &action, NULL) != 0) 
     {
        perror("Failed to install shutdown signal handlers");
        return EXIT_FAILURE;
    }

    // Opening parses the embedded ELF object and prepares its skeleton.
    // No program is running in the kernel at this stage.
    skeleton = exec_trace_bpf__open();
    if (skeleton == NULL) 
    {
        fprintf(stderr, "Failed to open the BPF object; see libbpf diagnostics.\n");
        return EXIT_FAILURE;
    }

    // Loading creates supporting maps and submits the program to the verifier.
    // A successful C compilation alone does not guarantee verifier acceptance.
    error = exec_trace_bpf__load(skeleton);
    if (error != 0) 
    {
        fprintf(stderr, "Failed to load the BPF program: %s (%d).\n", strerror(-error), error);
        goto cleanup;
    }

    // libbpf uses the SEC name to attach to syscalls:sys_enter_execve.
    // The skeleton owns the attachment; we deliberately do not pin it.
    error = exec_trace_bpf__attach(skeleton);
    if (error != 0) 
    {
        fprintf(stderr, "Failed to attach the execve tracepoint: %s (%d).\n", strerror(-error), error);
        goto cleanup;
    }

    puts("Tracing execve attempts. Press Ctrl+C to detach.");
    puts("Read p1_exec records in a second terminal from:");
    puts("  /sys/kernel/tracing/trace_pipe");
    fflush(stdout);

    // Sleeping avoids a busy loop. The handler changes the flag, and an
    // interrupted sleep or the next iteration brings us to normal cleanup.
    while (!stop_requested)
    {
        sleep(1);
    }

    status = EXIT_SUCCESS;
    puts("Detaching the execve tracer.");

cleanup:
    // Destroying closes the attachment and object descriptors, including any
    // resources created before a load/attach failure. No persistent pins remain.
    exec_trace_bpf__destroy(skeleton);
    return status;
}
