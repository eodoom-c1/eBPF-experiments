#define main request_service_program_main
#include "../request-service.c"
#undef main

int main(void)
{
    unsigned char field[8];
    uint64_t key = UINT64_C(0xfedcba9876543210);
    encode_u64(field, key);
    if (decode_u64(field) != key || field[0] != 0xfe || field[7] != 0x10)
    {
        exit_with_error("Network-order request fields do not round-trip");
    }
    unsigned char *values = malloc(KEY_COUNT * VALUE_BYTES);
    if (values == NULL)
    {
        exit_with_error("Cannot allocate test values");
    }
    initialize_values(values);
    const uint64_t keys[] = {0, 1, KEY_COUNT - 1};
    for (size_t index = 0; index < sizeof(keys) / sizeof(keys[0]); ++index)
    {
        const unsigned char *value = values + keys[index] * VALUE_BYTES;
        if (decode_u64(value) != keys[index] ||
            decode_u64(value + 8) != (keys[index] ^ UINT64_C(0x9e3779b97f4a7c15)))
        {
            exit_with_error("Lookup value does not identify its key");
        }
    }
    free(values);

    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0)
    {
        exit_with_error("Test socketpair: %s", strerror(errno));
    }
    unsigned char request[REQUEST_BYTES] = {0};
    if (send(sockets[0], request, 8, 0) != 8 || shutdown(sockets[0], SHUT_WR) != 0)
    {
        exit_with_error("Cannot supply truncated test request");
    }
    int connection_error = 0;
    if (read_request(sockets[1], request, &connection_error) != 0 || connection_error != EPROTO)
    {
        exit_with_error("Truncated request was not reported");
    }
    if (close(sockets[0]) != 0 || close(sockets[1]) != 0)
    {
        exit_with_error("Cannot close test sockets");
    }
    return EXIT_SUCCESS;
}
