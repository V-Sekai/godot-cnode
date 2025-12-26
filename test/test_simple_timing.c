#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/select.h>
#include <errno.h>
#include "ei.h"

#define MAXATOMLEN 255

static void log_time(const char* event) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    printf("[%ld.%06ld] %s\n", tv.tv_sec, tv.tv_usec, event);
    fflush(stdout);
}

int main(int argc, char** argv) {
    const char* cookie = "godotcookie";
    const char* node_name = "simple_test@127.0.0.1";
    int listen_fd;
    ei_cnode ec;

    log_time("Starting simple CNode test");

    /* Initialize erl_interface */
    log_time("Calling ei_init()...");
    if (ei_init() < 0) {
        fprintf(stderr, "ei_init failed\n");
        return 1;
    }
    log_time("ei_init() succeeded");

    /* Create CNode - with timeout check */
    log_time("Calling ei_connect_init()...");
    struct timeval connect_start, connect_end;
    gettimeofday(&connect_start, NULL);

    if (ei_connect_init(&ec, node_name, cookie, 0) < 0) {
        gettimeofday(&connect_end, NULL);
        long elapsed = (connect_end.tv_sec - connect_start.tv_sec) * 1000000 + (connect_end.tv_usec - connect_start.tv_usec);
        fprintf(stderr, "ei_connect_init failed after %ld us (errno: %d, %s)\n", elapsed, errno, strerror(errno));
        return 1;
    }
    gettimeofday(&connect_end, NULL);
    long elapsed = (connect_end.tv_sec - connect_start.tv_sec) * 1000000 + (connect_end.tv_usec - connect_start.tv_usec);
    log_time("ei_connect_init() succeeded");
    printf("ei_connect_init took %ld us (%.3f seconds)\n", elapsed, elapsed / 1000000.0);
    fflush(stdout);

    /* Harsh 5 second timeout: if ei_connect_init took more than 5 seconds, abort */
    if (elapsed > 5000000) {
        fprintf(stderr, "ERROR: ei_connect_init took %.3f seconds (> 5 seconds) - this is too slow!\n", elapsed / 1000000.0);
        return 1;
    }

    /* Create listening socket */
    log_time("Calling ei_listen()...");
    int port = 0;
    listen_fd = ei_listen(&ec, &port, 5); /* backlog = 5 */
    if (listen_fd < 0) {
        fprintf(stderr, "ei_listen failed (errno: %d, %s)\n", errno, strerror(errno));
        return 1;
    }

    log_time("Created listening socket");
    printf("Listening on port: %d\n", port);
    fflush(stdout);

    /* Publish with epmd */
    if (ei_publish(&ec, port) < 0) {
        fprintf(stderr, "ei_publish failed (errno: %d, %s)\n", errno, strerror(errno));
        /* Continue anyway - might be macOS issue */
    } else {
        log_time("Published with epmd");
    }

    /* Register global name */
    erlang_pid *self_pid = ei_self(&ec);
    if (self_pid == NULL) {
        fprintf(stderr, "ei_self failed\n");
    } else if (ei_global_register(listen_fd, "simple_test", self_pid) < 0) {
        fprintf(stderr, "ei_global_register failed (errno: %d, %s)\n", errno, strerror(errno));
    } else {
        log_time("Registered global name 'simple_test'");
    }

    printf("Ready to accept connections. Node: %s\n", node_name);
    fflush(stdout);

    /* Main loop: accept connections and process messages */
    int message_count = 0;
    struct timeval loop_start;
    gettimeofday(&loop_start, NULL);

    while (message_count < 10) { /* Process up to 10 messages */
        /* Harsh 5 second timeout: if we've been running for more than 5 seconds without a message, abort */
        struct timeval loop_now;
        gettimeofday(&loop_now, NULL);
        long loop_elapsed = (loop_now.tv_sec - loop_start.tv_sec) * 1000000 + (loop_now.tv_usec - loop_start.tv_usec);
        if (loop_elapsed > 5000000 && message_count == 0) {
            fprintf(stderr, "ERROR: No messages received within 5 seconds (elapsed: %.3f seconds) - TIMEOUT!\n", loop_elapsed / 1000000.0);
            break;
        }

        log_time("Waiting for connection...");

        /* Use select() to check for pending connections - harsh 1 second timeout */
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(listen_fd, &read_fds);
        struct timeval timeout;
        timeout.tv_sec = 1; /* Harsh: only wait 1 second for connection */
        timeout.tv_usec = 0;

        int select_res = select(listen_fd + 1, &read_fds, NULL, NULL, &timeout);
        if (select_res <= 0) {
            if (select_res == 0) {
                log_time("select() timeout - no connection pending (1 second timeout)");
            } else {
                log_time("select() error");
            }
            continue;
        }

        /* Accept connection */
        ErlConnect con;
        int fd = ei_accept(&ec, listen_fd, &con);
        if (fd < 0) {
            fprintf(stderr, "ei_accept failed (errno: %d, %s)\n", errno, strerror(errno));
            continue;
        }

        log_time("Accepted connection");
        if (con.nodename[0] != '\0') {
            printf("Connected from: %s\n", con.nodename);
        }
        fflush(stdout);

        /* Receive and process messages on this connection */
        ei_x_buff x;
        ei_x_new(&x);
        erlang_msg msg;

        int messages_on_connection = 0;
        while (messages_on_connection < 5) { /* Process up to 5 messages per connection */
            log_time("Waiting to receive message...");

            /* Use select() to wait for data - harsh 1 second timeout */
            FD_ZERO(&read_fds);
            FD_SET(fd, &read_fds);
            timeout.tv_sec = 1; /* Harsh: only wait 1 second for data */
            timeout.tv_usec = 0;

            struct timeval recv_start, recv_end;
            gettimeofday(&recv_start, NULL);
            select_res = select(fd + 1, &read_fds, NULL, NULL, &timeout);
            gettimeofday(&recv_end, NULL);
            long recv_wait = (recv_end.tv_sec - recv_start.tv_sec) * 1000000 + (recv_end.tv_usec - recv_start.tv_usec);

            if (select_res <= 0) {
                if (select_res == 0) {
                    log_time("select() timeout - no data available (1 second timeout)");
                    printf("Waited %ld us for data\n", recv_wait);
                } else {
                    log_time("select() error");
                }
                break; /* No more data, close connection */
            }

            log_time("Data available, calling ei_receive_msg...");
            gettimeofday(&recv_start, NULL);
            int res = ei_receive_msg(fd, &msg, &x);
            gettimeofday(&recv_end, NULL);
            long recv_elapsed = (recv_end.tv_sec - recv_start.tv_sec) * 1000000 + (recv_end.tv_usec - recv_start.tv_usec);
            printf("ei_receive_msg took %ld us (%.3f seconds)\n", recv_elapsed, recv_elapsed / 1000000.0);
            fflush(stdout);

            /* Harsh timeout: if ei_receive_msg took more than 1 second, it's too slow */
            if (recv_elapsed > 1000000) {
                fprintf(stderr, "ERROR: ei_receive_msg took %.3f seconds (> 1 second) - this is too slow!\n", recv_elapsed / 1000000.0);
            }

            if (res == ERL_TICK) {
                log_time("Received ERL_TICK (keepalive)");
                continue;
            } else if (res == ERL_ERROR) {
                int saved_errno = errno;
                fprintf(stderr, "ei_receive_msg failed (errno: %d, %s)\n", saved_errno, strerror(saved_errno));

                /* Try raw read as fallback */
                if (saved_errno == 42 || saved_errno == 60 || saved_errno == ENOPROTOOPT || saved_errno == ETIMEDOUT) {
                    log_time("Attempting raw read fallback...");
                    unsigned char raw_buf[4096];
                    ssize_t bytes_read = read(fd, raw_buf, sizeof(raw_buf));
                    if (bytes_read > 0) {
                        printf("Raw read got %zd bytes\n", bytes_read);
                        fflush(stdout);
                        /* For now, just log it - we'll decode later if needed */
                        break;
                    }
                }
                break;
            } else {
                log_time("ei_receive_msg succeeded");
                message_count++;
                messages_on_connection++;

                printf("Message #%d (connection message #%d), type: %ld\n",
                       message_count, messages_on_connection, msg.msgtype);
                fflush(stdout);

                /* Decode and process message */
                int index = 0;
                int version;
                if (ei_decode_version(x.buff, &index, &version) < 0) {
                    fprintf(stderr, "Failed to decode version\n");
                    break;
                }

                int tuple_arity;
                if (ei_decode_tuple_header(x.buff, &index, &tuple_arity) < 0) {
                    fprintf(stderr, "Failed to decode tuple header\n");
                    break;
                }

                char first_atom[MAXATOMLEN];
                if (ei_decode_atom(x.buff, &index, first_atom) < 0) {
                    fprintf(stderr, "Failed to decode first atom\n");
                    break;
                }

                printf("Message: tuple arity=%d, first atom='%s'\n", tuple_arity, first_atom);
                fflush(stdout);

                /* If it's a gen_call, send a reply */
                if (strcmp(first_atom, "$gen_call") == 0) {
                    log_time("Processing gen_call, sending reply...");

                    /* Decode {From, Tag} */
                    int from_arity;
                    if (ei_decode_tuple_header(x.buff, &index, &from_arity) < 0 || from_arity != 2) {
                        fprintf(stderr, "Failed to decode From tuple\n");
                        break;
                    }

                    erlang_pid from_pid;
                    erlang_ref tag_ref;
                    if (ei_decode_pid(x.buff, &index, &from_pid) < 0) {
                        fprintf(stderr, "Failed to decode From PID\n");
                        break;
                    }
                    if (ei_decode_ref(x.buff, &index, &tag_ref) < 0) {
                        fprintf(stderr, "Failed to decode Tag\n");
                        break;
                    }

                    /* Decode Request: {Module, Function, Args} */
                    int req_arity;
                    if (ei_decode_tuple_header(x.buff, &index, &req_arity) < 0 || req_arity < 2) {
                        fprintf(stderr, "Failed to decode Request tuple\n");
                        break;
                    }

                    char module[MAXATOMLEN], function[MAXATOMLEN];
                    if (ei_decode_atom(x.buff, &index, module) < 0) {
                        fprintf(stderr, "Failed to decode Module\n");
                        break;
                    }
                    if (ei_decode_atom(x.buff, &index, function) < 0) {
                        fprintf(stderr, "Failed to decode Function\n");
                        break;
                    }

                    printf("Request: %s:%s\n", module, function);
                    fflush(stdout);

                    /* Build reply */
                    ei_x_buff reply;
                    ei_x_new_with_version(&reply);

                    /* Reply format: {Tag, Reply} */
                    ei_x_encode_tuple_header(&reply, 2);
                    ei_x_encode_ref(&reply, &tag_ref);

                    /* For erlang:node, return the node name */
                    if (strcmp(module, "erlang") == 0 && strcmp(function, "node") == 0) {
                        ei_x_encode_atom(&reply, ec.thisnodename);
                    } else {
                        /* Error reply */
                        ei_x_encode_tuple_header(&reply, 2);
                        ei_x_encode_atom(&reply, "error");
                        ei_x_encode_string(&reply, "unknown_function");
                    }

                    /* Send reply */
                    log_time("Sending reply...");
                    int send_res = ei_send(fd, &from_pid, reply.buff, reply.index);
                    if (send_res < 0) {
                        fprintf(stderr, "ei_send failed (errno: %d, %s)\n", errno, strerror(errno));
                    } else {
                        log_time("Reply sent successfully");
                        printf("Reply sent: %d bytes\n", reply.index);
                        fflush(stdout);
                    }

                    ei_x_free(&reply);
                }

                /* Prepare for next message */
                ei_x_free(&x);
                ei_x_new(&x);
            }
        }

        log_time("Closing connection");
        ei_x_free(&x);
        close(fd);
    }

    log_time("Exiting");
    close(listen_fd);
    return 0;
}
