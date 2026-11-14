/**
 * @file server.c
 * @brief IPC fabric server for the Cog PoC.
 *
 * The server binds a Unix domain socket, accepts multiple clients, and
 * uses poll(2) to multiplex I/O. Each client is assigned a numeric ID;
 * messages received from a client are echoed back prefixed with that ID.
 */

#define _POSIX_C_SOURCE 200809L

#include "common.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

/** Initial capacity of the client table. */
#define CLIENT_TABLE_INITIAL 8U

/** Maximum number of pending connections for listen(2). */
#define LISTEN_BACKLOG 8

/** States for the per-client frame parser. */
enum frame_state {
    FRAME_STATE_HEADER = 0,
    FRAME_STATE_PAYLOAD
};

/** Per-client connection state. */
struct client {
    int fd;
    uint32_t id;
    enum frame_state state;
    uint8_t header[IPC_HEADER_SIZE];
    size_t header_len;
    uint8_t *payload;
    size_t payload_len;
    size_t payload_received;
};

/** Dynamic table of connected clients. */
struct client_table {
    struct client *clients;
    size_t count;
    size_t capacity;
    uint32_t next_id;
};

/**
 * @brief Initialise an empty client table.
 *
 * @param[out] table Table to initialise.
 * @return 0 on success, -1 on allocation failure.
 */
static int client_table_init(struct client_table *table)
{
    table->clients = calloc(CLIENT_TABLE_INITIAL, sizeof(table->clients[0]));
    if (table->clients == NULL) {
        return -1;
    }
    table->count = 0U;
    table->capacity = CLIENT_TABLE_INITIAL;
    table->next_id = 1U;
    return 0;
}

/**
 * @brief Release all resources held by a client table.
 *
 * @param[in,out] table Table to destroy.
 */
static void client_table_destroy(struct client_table *table)
{
    for (size_t i = 0U; i < table->count; i++) {
        struct client *c = &table->clients[i];
        if (c->fd >= 0) {
            (void)close(c->fd);
        }
        free(c->payload);
    }
    free(table->clients);
    table->clients = NULL;
    table->count = 0U;
    table->capacity = 0U;
}

/**
 * @brief Grow the client table if it is full.
 *
 * @param[in,out] table Table to grow.
 * @return 0 on success, -1 on allocation failure.
 */
static int client_table_grow(struct client_table *table)
{
    size_t new_capacity;
    struct client *new_clients;

    if (table->count < table->capacity) {
        return 0;
    }

    new_capacity = table->capacity * 2U;
    new_clients = realloc(table->clients,
                          new_capacity * sizeof(table->clients[0]));
    if (new_clients == NULL) {
        return -1;
    }

    /* Zero the newly allocated slots. */
    (void)memset(&new_clients[table->capacity], 0,
                 (new_capacity - table->capacity) * sizeof(new_clients[0]));

    table->clients = new_clients;
    table->capacity = new_capacity;
    return 0;
}

/**
 * @brief Remove a client from the table by index.
 *
 * @param[in,out] table Table to modify.
 * @param[in] index Index of the client to remove.
 */
static void client_remove(struct client_table *table, size_t index)
{
    struct client *c = &table->clients[index];

    if (c->fd >= 0) {
        (void)close(c->fd);
    }
    free(c->payload);

    if (index < table->count - 1U) {
        (void)memmove(&table->clients[index],
                      &table->clients[index + 1U],
                      (table->count - index - 1U) * sizeof(table->clients[0]));
    }

    table->count--;
}

/**
 * @brief Reset a client to the header-reading state, freeing any payload.
 *
 * @param[in,out] c Client to reset.
 */
static void client_reset_frame(struct client *c)
{
    c->state = FRAME_STATE_HEADER;
    c->header_len = 0U;
    free(c->payload);
    c->payload = NULL;
    c->payload_len = 0U;
    c->payload_received = 0U;
}

/**
 * @brief Accept a new client connection.
 *
 * @param[in] listen_fd Listening socket.
 * @param[in,out] table Client table.
 * @return 0 on success, -1 on error.
 */
static int client_accept(int listen_fd, struct client_table *table)
{
    int fd;
    struct client *c;

    fd = accept(listen_fd, NULL, NULL);
    if (fd < 0) {
        return -1;
    }

    if (client_table_grow(table) < 0) {
        (void)close(fd);
        return -1;
    }

    c = &table->clients[table->count];
    c->fd = fd;
    c->id = table->next_id++;
    c->state = FRAME_STATE_HEADER;
    c->header_len = 0U;
    c->payload = NULL;
    c->payload_len = 0U;
    c->payload_received = 0U;
    table->count++;

    (void)printf("client %u connected (fd %d)\n", c->id, fd);
    return 0;
}

/**
 * @brief Dispatch a complete message from a client.
 *
 * Echoes the payload back to the same client prefixed with its client ID.
 *
 * @param[in,out] c Client that sent the message.
 * @return 0 on success, -1 on error.
 */
static int client_dispatch_message(struct client *c)
{
    uint8_t header[IPC_HEADER_SIZE];
    char prefix[32];
    size_t prefix_len;
    uint32_t reply_len;

    (void)snprintf(prefix, sizeof(prefix), "[%u] ", c->id);
    prefix_len = strlen(prefix);

    if (c->payload_len > IPC_MAX_PAYLOAD - prefix_len) {
        (void)fprintf(stderr, "client %u: reply too large, dropping\n", c->id);
        return 0;
    }

    reply_len = (uint32_t)(prefix_len + c->payload_len);
    ipc_frame_encode_header(header, reply_len);

    if (ipc_write_exact(c->fd, header, IPC_HEADER_SIZE) < 0) {
        return -1;
    }
    if (ipc_write_exact(c->fd, prefix, prefix_len) < 0) {
        return -1;
    }
    if (c->payload_len > 0U) {
        if (ipc_write_exact(c->fd, c->payload, c->payload_len) < 0) {
            return -1;
        }
    }

    (void)printf("client %u sent %zu bytes\n", c->id, c->payload_len);
    return 0;
}

/**
 * @brief Read available bytes from a client and advance its frame parser.
 *
 * @param[in,out] c Client to read from.
 * @return 0 if the client is still healthy, -1 if it should be removed.
 */
static int client_read(struct client *c)
{
    uint8_t buf[1024];
    ssize_t rc;

    rc = read(c->fd, buf, sizeof(buf));
    if (rc < 0) {
        if (errno == EINTR) {
            return 0;
        }
        return -1;
    }
    if (rc == 0) {
        return -1;
    }

    for (ssize_t i = 0; i < rc; i++) {
        uint8_t byte = buf[i];

        if (c->state == FRAME_STATE_HEADER) {
            c->header[c->header_len++] = byte;
            if (c->header_len == IPC_HEADER_SIZE) {
                c->payload_len = ipc_frame_decode_header(c->header);
                if (c->payload_len > IPC_MAX_PAYLOAD) {
                    (void)fprintf(stderr,
                                  "client %u: oversized frame (%zu), closing\n",
                                  c->id, c->payload_len);
                    return -1;
                }
                if (c->payload_len == 0U) {
                    c->payload = NULL;
                    if (client_dispatch_message(c) < 0) {
                        return -1;
                    }
                    client_reset_frame(c);
                } else {
                    c->payload = malloc(c->payload_len);
                    if (c->payload == NULL) {
                        return -1;
                    }
                    c->payload_received = 0U;
                    c->state = FRAME_STATE_PAYLOAD;
                }
            }
        } else {
            c->payload[c->payload_received++] = byte;
            if (c->payload_received == c->payload_len) {
                if (client_dispatch_message(c) < 0) {
                    return -1;
                }
                client_reset_frame(c);
            }
        }
    }

    return 0;
}

/**
 * @brief Build a pollfd array from the listening socket and client table.
 *
 * @param[in] listen_fd Listening socket.
 * @param[in] table Client table.
 * @param[out] fds Array to fill.
 * @param[in] fds_size Capacity of @p fds.
 * @return Number of entries written, or -1 if @p fds is too small.
 */
static int build_poll_fds(int listen_fd, const struct client_table *table,
                          struct pollfd *fds, size_t fds_size)
{
    if (fds_size < table->count + 1U) {
        return -1;
    }

    fds[0].fd = listen_fd;
    fds[0].events = POLLIN;
    fds[0].revents = 0;

    for (size_t i = 0U; i < table->count; i++) {
        fds[i + 1U].fd = table->clients[i].fd;
        fds[i + 1U].events = POLLIN;
        fds[i + 1U].revents = 0;
    }

    return (int)(table->count + 1U);
}

/**
 * @brief Create and bind the Unix domain socket.
 *
 * @param[in] path Socket path.
 * @return Listening file descriptor, or -1 on error.
 */
static int create_server_socket(const char *path)
{
    int fd;
    struct sockaddr_un addr;
    size_t path_len;

    (void)unlink(path);

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }

    (void)memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    path_len = strlen(path);
    if (path_len >= sizeof(addr.sun_path)) {
        (void)fprintf(stderr, "socket path too long\n");
        (void)close(fd);
        return -1;
    }
    (void)strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1U);

    if (bind(fd, (const struct sockaddr *)&addr, sizeof(addr)) < 0) {
        (void)close(fd);
        return -1;
    }

    if (listen(fd, LISTEN_BACKLOG) < 0) {
        (void)close(fd);
        return -1;
    }

    return fd;
}

/** @brief Global flag set by the SIGINT handler for graceful shutdown. */
static volatile sig_atomic_t g_running = 1;

/**
 * @brief SIGINT handler: request graceful shutdown.
 *
 * @param[in] signum Signal number (unused).
 */
static void handle_sigint(int signum)
{
    (void)signum;
    g_running = 0;
}

int
main(int argc, char **argv)
{
    const char *path;
    int listen_fd;
    struct client_table table;
    struct pollfd *fds;
    size_t fds_capacity;
    int nfds;
    int rc;

    (void)argc;
    (void)argv;

    path = ipc_socket_path();
    listen_fd = create_server_socket(path);
    if (listen_fd < 0) {
        (void)perror("create_server_socket");
        return EXIT_FAILURE;
    }

    (void)printf("ipc-server listening on %s\n", path);

    if (client_table_init(&table) < 0) {
        (void)close(listen_fd);
        (void)unlink(path);
        return EXIT_FAILURE;
    }

    fds_capacity = CLIENT_TABLE_INITIAL + 1U;
    fds = calloc(fds_capacity, sizeof(fds[0]));
    if (fds == NULL) {
        client_table_destroy(&table);
        (void)close(listen_fd);
        (void)unlink(path);
        return EXIT_FAILURE;
    }

    (void)signal(SIGINT, handle_sigint);
    (void)signal(SIGTERM, handle_sigint);

    while (g_running != 0) {
        if (fds_capacity < table.count + 1U) {
            size_t new_capacity = (table.count + 1U) * 2U;
            struct pollfd *new_fds = realloc(fds, new_capacity * sizeof(fds[0]));
            if (new_fds == NULL) {
                break;
            }
            fds = new_fds;
            fds_capacity = new_capacity;
        }

        nfds = build_poll_fds(listen_fd, &table, fds, fds_capacity);
        if (nfds < 0) {
            break;
        }

        rc = poll(fds, (nfds_t)nfds, -1);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            (void)perror("poll");
            break;
        }

        /* Process existing clients first. The pollfd array matches the
         * client table as it existed before this poll call; accepting new
         * clients happens afterwards so indices stay consistent. */
        for (size_t i = table.count; i > 0U; i--) {
            size_t idx = i - 1U;
            struct client *c = &table.clients[idx];
            short revents = fds[idx + 1U].revents;

            if ((revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                (void)printf("client %u disconnected (event %hd)\n",
                             c->id, revents);
                client_remove(&table, idx);
                continue;
            }

            if ((revents & POLLIN) != 0) {
                if (client_read(c) < 0) {
                    (void)printf("client %u disconnected\n", c->id);
                    client_remove(&table, idx);
                }
            }
        }

        /* Accept any new connections after processing existing clients. */
        if ((fds[0].revents & POLLIN) != 0) {
            (void)client_accept(listen_fd, &table);
        }
    }

    (void)printf("shutting down\n");

    free(fds);
    client_table_destroy(&table);
    (void)close(listen_fd);
    (void)unlink(path);

    return EXIT_SUCCESS;
}
