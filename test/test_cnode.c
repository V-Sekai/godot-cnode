/*
 * Standalone C CNode with Full GenServer Features
 *
 * This is a pure C implementation of a CNode that supports:
 * - GenServer Call (synchronous RPC with replies)
 * - GenServer Cast (asynchronous messages, no reply)
 * - Global name registration
 * - Multiple handlers (erlang:node, erlang:nodes, test:ping, test:echo)
 *
 * Used for debugging reply issues by comparing with Godot CNode behavior.
 */

#define _POSIX_C_SOURCE 200112L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <time.h>
#include <fcntl.h>

// erl_interface headers
#include "ei.h"
#include "ei_connect.h"

/* Forward declaration - ei_default_socket_callbacks is not in public headers */
extern ei_socket_callbacks ei_default_socket_callbacks;

// Global CNode state
static ei_cnode ec;
static int listen_fd = -1;
static int publish_fd = -1;

// Forward declarations
static int init_cnode(const char *nodename, const char *cookie);
static int process_message(char *buf, int *index, int fd);
static int handle_call(char *buf, int *index, int fd, erlang_pid *from_pid, erlang_ref *tag_ref);
static int handle_cast(char *buf, int *index);
static void send_reply(ei_x_buff *x, int fd, erlang_pid *to_pid, erlang_ref *tag_ref);
static void main_loop(void);

/* Custom socket callbacks for macOS compatibility */
/* macOS doesn't support SO_ACCEPTCONN, so we need a custom accept implementation */

/* Helper to extract FD from context */
#define EI_DFLT_CTX_TO_FD__(CTX, FD)      \
	((intptr_t)(CTX) < 0                  \
					? (*(FD) = -1, EBADF) \
					: (*(FD) = (int)(intptr_t)(CTX), 0))

/* Helper to convert FD to context */
#define EI_FD_AS_CTX__(FD) ((void *)(intptr_t)(FD))

/* macOS-compatible accept callback */
static int macos_tcp_accept(void **ctx, void *addr, int *len, unsigned __attribute__((unused)) unused) {
	int fd, res;
	socklen_t addr_len = (socklen_t)*len;

	if (!ctx)
		return EINVAL;

	/* Extract file descriptor from context */
	res = EI_DFLT_CTX_TO_FD__(*ctx, &fd);
	if (res)
		return res;

	/* Ensure socket is in blocking mode (not non-blocking) */
	int flags = fcntl(fd, F_GETFL, 0);
	if (flags >= 0) {
		fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
	}

	/* Call accept() directly */
	res = accept(fd, (struct sockaddr *)addr, &addr_len);
	if (res < 0) {
		return errno;
	}

	*len = (int)addr_len;

	/* Store accepted socket in context */
	*ctx = EI_FD_AS_CTX__(res);
	return 0;
}

/* Wrapper functions that delegate to default callbacks */
static int custom_socket(void **ctx, void *setup_ctx) {
	return ei_default_socket_callbacks.socket(ctx, setup_ctx);
}

static int custom_close(void *ctx) {
	return ei_default_socket_callbacks.close(ctx);
}

static int custom_listen(void *ctx, void *addr, int *len, int backlog) {
	return ei_default_socket_callbacks.listen(ctx, addr, len, backlog);
}

static int custom_connect(void *ctx, void *addr, int len, unsigned tmo) {
	return ei_default_socket_callbacks.connect(ctx, addr, len, tmo);
}

static int custom_writev(void *ctx, const void *iov, int iovcnt, ssize_t *len, unsigned tmo) {
	if (ei_default_socket_callbacks.writev) {
		return ei_default_socket_callbacks.writev(ctx, iov, iovcnt, len, tmo);
	}
	return ENOTSUP;
}

static int custom_write(void *ctx, const char *buf, ssize_t *len, unsigned tmo) {
	return ei_default_socket_callbacks.write(ctx, buf, len, tmo);
}

static int custom_read(void *ctx, char *buf, ssize_t *len, unsigned tmo) {
	return ei_default_socket_callbacks.read(ctx, buf, len, tmo);
}

static int custom_handshake_packet_header_size(void *ctx, int *sz) {
	return ei_default_socket_callbacks.handshake_packet_header_size(ctx, sz);
}

static int custom_connect_handshake_complete(void *ctx) {
	return ei_default_socket_callbacks.connect_handshake_complete(ctx);
}

static int custom_accept_handshake_complete(void *ctx) {
	return ei_default_socket_callbacks.accept_handshake_complete(ctx);
}

static int custom_get_fd(void *ctx, int *fd) {
	return ei_default_socket_callbacks.get_fd(ctx, fd);
}

/* Custom socket callbacks structure */
static ei_socket_callbacks custom_socket_callbacks = {
	0, /* flags */
	custom_socket,
	custom_close,
	custom_listen,
	macos_tcp_accept, /* accept - custom macOS-compatible implementation */
	custom_connect,
#ifdef __APPLE__
#ifdef EI_HAVE_STRUCT_IOVEC__
	custom_writev,
#else
	NULL,
#endif
#else
	custom_writev,
#endif
	custom_write,
	custom_read,
	custom_handshake_packet_header_size,
	custom_connect_handshake_complete,
	custom_accept_handshake_complete,
	custom_get_fd
};

/*
 * Initialize the CNode
 */
static int init_cnode(const char *nodename, const char *cookie) {
	int res;
	int fd;
	int port = 0;

	/* Zero-initialize the ei_cnode structure */
	memset(&ec, 0, sizeof(ei_cnode));

	/* Validate inputs */
	if (nodename == NULL || strlen(nodename) == 0) {
		fprintf(stderr, "init_cnode: invalid nodename (null or empty)\n");
		return -1;
	}

	if (strchr(nodename, '@') == NULL) {
		fprintf(stderr, "init_cnode: invalid nodename format (must be 'name@hostname'): %s\n", nodename);
		return -1;
	}

	if (cookie == NULL || strlen(cookie) == 0) {
		fprintf(stderr, "init_cnode: invalid cookie (null or empty)\n");
		return -1;
	}

	/* Initialize ei library */
	ei_init();

	/* Extract hostname and alivename from nodename */
	char thishostname[EI_MAXHOSTNAMELEN + 1] = { 0 };
	char thisalivename[EI_MAXALIVELEN + 1] = { 0 };
	char thisnodename[MAXNODELEN + 1] = { 0 };

	char *at_pos = strchr(nodename, '@');
	if (at_pos == NULL) {
		fprintf(stderr, "init_cnode: invalid nodename format: %s\n", nodename);
		return -1;
	}

	/* Extract alivename (part before @) */
	size_t alivename_len = at_pos - nodename;
	if (alivename_len >= sizeof(thisalivename)) {
		alivename_len = sizeof(thisalivename) - 1;
	}
	strncpy(thisalivename, nodename, alivename_len);
	thisalivename[alivename_len] = '\0';

	/* Extract hostname (part after @) */
	const char *hostname = at_pos + 1;
	if (strlen(hostname) >= sizeof(thishostname)) {
		fprintf(stderr, "init_cnode: hostname too long: %s\n", hostname);
		return -1;
	}
	strncpy(thishostname, hostname, sizeof(thishostname) - 1);
	thishostname[sizeof(thishostname) - 1] = '\0';

	/* Full nodename */
	if (strlen(nodename) >= sizeof(thisnodename)) {
		fprintf(stderr, "init_cnode: nodename too long: %s\n", nodename);
		return -1;
	}
	strncpy(thisnodename, nodename, sizeof(thisnodename) - 1);
	thisnodename[sizeof(thisnodename) - 1] = '\0';

	/* Use ei_connect_xinit_ussi with custom socket callbacks for macOS compatibility */
	res = ei_connect_xinit_ussi(&ec, thishostname, thisalivename, thisnodename,
			NULL, /* thisipaddr - not used */
			cookie, 0, /* creation */
			&custom_socket_callbacks,
			sizeof(custom_socket_callbacks),
			NULL); /* setup_context */
	if (res < 0) {
		fprintf(stderr, "ei_connect_xinit_ussi failed: %d (errno: %d, %s)\n", res, errno, strerror(errno));
		fprintf(stderr, "  nodename: %s\n", nodename);
		fprintf(stderr, "  thishostname: %s\n", thishostname);
		fprintf(stderr, "  thisalivename: %s\n", thisalivename);
		return -1;
	}

	/* Create listening socket */
	fd = ei_listen(&ec, &port, 5);
	if (fd < 0) {
		fprintf(stderr, "ei_listen failed: %d (errno: %d, %s)\n", fd, errno, strerror(errno));
		return -1;
	}
	printf("Test CNode: Created listening socket on port %d\n", port);
	fflush(stdout);

	/* Register with epmd */
	publish_fd = ei_publish(&ec, port);
	if (publish_fd < 0) {
		fprintf(stderr, "ei_publish failed: %d (errno: %d, %s)\n", publish_fd, errno, strerror(errno));
		fflush(stderr);
		if (errno == ECONNREFUSED || errno == 61) {
			fprintf(stderr, "  epmd is not running. Start with 'epmd -daemon'\n");
			fflush(stderr);
			close(fd);
			return -1;
		}
		close(fd);
		return -1;
	}
	printf("Test CNode: Successfully published node with epmd on port %d (publish_fd: %d)\n", port, publish_fd);
	fflush(stdout);

	listen_fd = fd;
	printf("Test CNode: Ready for connections (fd: %d, port: %d)\n", listen_fd, port);
	fflush(stdout);
	return 0;
}

/*
 * Process incoming message from Erlang/Elixir
 */
static int process_message(char *buf, int *index, int fd) {
	int version;
	int arity;
	char atom[MAXATOMLEN];
	int saved_index = *index;

	/* Decode version (optional) */
	if (ei_decode_version(buf, index, &version) < 0) {
		*index = saved_index;
	} else {
		saved_index = *index;
	}

	/* Decode tuple header */
	if (ei_decode_tuple_header(buf, index, &arity) < 0) {
		fprintf(stderr, "Error decoding tuple header\n");
		return -1;
	}

	/* Check if this is a GenServer-style message */
	int tuple_start_index = *index;
	if (ei_decode_atom(buf, index, atom) < 0) {
		fprintf(stderr, "Error decoding atom\n");
		return -1;
	}

	/* Handle GenServer call */
	if (strcmp(atom, "$gen_call") == 0) {
		/* GenServer call: {'$gen_call', {From, Tag}, Request} */
		int from_arity;
		if (ei_decode_tuple_header(buf, index, &from_arity) < 0 || from_arity != 2) {
			fprintf(stderr, "Error decoding From tuple in gen_call\n");
			return -1;
		}

		erlang_pid from_pid;
		erlang_ref tag_ref;
		if (ei_decode_pid(buf, index, &from_pid) < 0) {
			fprintf(stderr, "Error decoding From PID in gen_call\n");
			return -1;
		}
		if (ei_decode_ref(buf, index, &tag_ref) < 0) {
			fprintf(stderr, "Error decoding Tag in gen_call\n");
			return -1;
		}

		printf("Test CNode: Received GenServer call (synchronous RPC with reply)\n");
		return handle_call(buf, index, fd, &from_pid, &tag_ref);
	}
	/* Handle GenServer cast */
	else if (strcmp(atom, "$gen_cast") == 0) {
		/* GenServer cast: {'$gen_cast', Request} */
		printf("Test CNode: Received GenServer cast (asynchronous, no reply)\n");
		return handle_cast(buf, index);
	}
	/* Handle rex format (for registered names) */
	else if (strcmp(atom, "rex") == 0) {
		/* RPC message format: {rex, From, Request} where Request is {'$gen_call', ...} */
		printf("Test CNode: Received RPC message (rex format)\n");

		erlang_pid rpc_from_pid;
		if (ei_decode_pid(buf, index, &rpc_from_pid) < 0) {
			fprintf(stderr, "Error decoding From PID in rex message\n");
			return -1;
		}

		int request_arity;
		if (ei_decode_tuple_header(buf, index, &request_arity) < 0) {
			fprintf(stderr, "Error decoding Request tuple in rex message\n");
			return -1;
		}

		char gen_call_atom[MAXATOMLEN];
		if (ei_decode_atom(buf, index, gen_call_atom) < 0 || strcmp(gen_call_atom, "$gen_call") != 0) {
			fprintf(stderr, "Error: Request in rex message is not a gen_call (got: %s)\n", gen_call_atom);
			return -1;
		}

		int from_arity;
		if (ei_decode_tuple_header(buf, index, &from_arity) < 0 || from_arity != 2) {
			fprintf(stderr, "Error decoding From tuple in rex gen_call\n");
			return -1;
		}

		erlang_pid from_pid;
		erlang_ref tag_ref;
		if (ei_decode_pid(buf, index, &from_pid) < 0) {
			fprintf(stderr, "Error decoding From PID in rex gen_call\n");
			return -1;
		}
		if (ei_decode_ref(buf, index, &tag_ref) < 0) {
			fprintf(stderr, "Error decoding Tag in rex gen_call\n");
			return -1;
		}

		printf("Test CNode: Processing rex GenServer call\n");
		return handle_call(buf, index, fd, &from_pid, &tag_ref);
	}
	/* Handle plain message */
	else {
		/* Plain message: {Module, Function, Args} */
		*index = tuple_start_index;
		printf("Test CNode: Received plain message (asynchronous, no reply)\n");
		return handle_cast(buf, index);
	}
}

/*
 * Handle synchronous call (GenServer call with reply)
 */
static int handle_call(char *buf, int *index, int fd, erlang_pid *from_pid, erlang_ref *tag_ref) {
	ei_x_buff reply;
	ei_x_new(&reply);

	/* Decode Request: {Module, Function, Args} */
	int request_arity;
	if (ei_decode_tuple_header(buf, index, &request_arity) < 0 || request_arity < 2) {
		fprintf(stderr, "Error: invalid request format in gen_call\n");
		ei_x_encode_tuple_header(&reply, 2);
		ei_x_encode_atom(&reply, "error");
		ei_x_encode_string(&reply, "invalid_request_format");
		send_reply(&reply, fd, from_pid, tag_ref);
		ei_x_free(&reply);
		return -1;
	}

	/* Decode Module and Function */
	char module[256];
	char function[256];

	if (ei_decode_atom(buf, index, module) < 0) {
		fprintf(stderr, "Error decoding module in call\n");
		ei_x_encode_tuple_header(&reply, 2);
		ei_x_encode_atom(&reply, "error");
		ei_x_encode_string(&reply, "invalid_module");
		send_reply(&reply, fd, from_pid, tag_ref);
		ei_x_free(&reply);
		return -1;
	}

	if (ei_decode_atom(buf, index, function) < 0) {
		fprintf(stderr, "Error decoding function in call\n");
		ei_x_encode_tuple_header(&reply, 2);
		ei_x_encode_atom(&reply, "error");
		ei_x_encode_string(&reply, "invalid_function");
		send_reply(&reply, fd, from_pid, tag_ref);
		ei_x_free(&reply);
		return -1;
	}

	printf("Test CNode: handle_call - Module=%s, Function=%s\n", module, function);

	/* Route based on module */
	if (strcmp(module, "erlang") == 0) {
		if (strcmp(function, "node") == 0) {
			/* Return the CNode's nodename */
			ei_x_encode_atom(&reply, ec.thisnodename);
		} else if (strcmp(function, "nodes") == 0) {
			/* Return empty list of connected nodes */
			ei_x_encode_list_header(&reply, 0);
			ei_x_encode_empty_list(&reply);
		} else {
			ei_x_encode_tuple_header(&reply, 2);
			ei_x_encode_atom(&reply, "error");
			ei_x_encode_string(&reply, "unknown_function");
		}
	} else if (strcmp(module, "test") == 0) {
		if (strcmp(function, "ping") == 0) {
			/* Simple ping/pong */
			ei_x_encode_atom(&reply, "pong");
		} else if (strcmp(function, "echo") == 0) {
			/* Echo back arguments */
			if (request_arity > 2) {
				/* Decode and echo back the first argument */
				long arg;
				if (ei_decode_long(buf, index, &arg) == 0) {
					ei_x_encode_long(&reply, arg);
				} else {
					char str_arg[256];
					if (ei_decode_string(buf, index, str_arg) == 0) {
						ei_x_encode_string(&reply, str_arg);
					} else {
						ei_x_encode_atom(&reply, "echo");
					}
				}
			} else {
				ei_x_encode_atom(&reply, "echo");
			}
		} else {
			ei_x_encode_tuple_header(&reply, 2);
			ei_x_encode_atom(&reply, "error");
			ei_x_encode_string(&reply, "unknown_function");
		}
	} else {
		ei_x_encode_tuple_header(&reply, 2);
		ei_x_encode_atom(&reply, "error");
		ei_x_encode_string(&reply, "unknown_module");
	}

	/* Send reply */
	send_reply(&reply, fd, from_pid, tag_ref);
	ei_x_free(&reply);

	return 0;
}

/*
 * Handle asynchronous cast (GenServer cast, no reply)
 */
static int handle_cast(char *buf, int *index) {
	/* Decode Request: {Module, Function, Args} */
	int request_arity;
	if (ei_decode_tuple_header(buf, index, &request_arity) < 0 || request_arity < 2) {
		fprintf(stderr, "Error: invalid request format in gen_cast\n");
		return -1;
	}

	char module[256];
	char function[256];

	if (ei_decode_atom(buf, index, module) < 0) {
		fprintf(stderr, "Error decoding module in cast\n");
		return -1;
	}

	if (ei_decode_atom(buf, index, function) < 0) {
		fprintf(stderr, "Error decoding function in cast\n");
		return -1;
	}

	printf("Test CNode: Processing async message - Module: %s, Function: %s\n", module, function);

	/* Process the cast (no reply sent) */
	if (strcmp(module, "erlang") == 0 && strcmp(function, "node") == 0) {
		printf("Test CNode: Async erlang:node - Node name: %s\n", ec.thisnodename);
	} else if (strcmp(module, "test") == 0 && strcmp(function, "ping") == 0) {
		printf("Test CNode: Async test:ping received\n");
	}

	printf("Test CNode: Async message processing complete\n");
	return 0;
}

/*
 * Send reply to Erlang/Elixir (GenServer-style synchronous call)
 */
static void send_reply(ei_x_buff *x, int fd, erlang_pid *to_pid, erlang_ref *tag_ref) {
	if (x == NULL || fd < 0 || to_pid == NULL || tag_ref == NULL) {
		fprintf(stderr, "Error: invalid parameters in send_reply\n");
		return;
	}

	/* Encode GenServer-style reply: {Tag, Reply} */
	ei_x_buff gen_reply;
	ei_x_new_with_version(&gen_reply);

	/* Encode tuple header for {Tag, Reply} */
	ei_x_encode_tuple_header(&gen_reply, 2);

	/* Encode the tag (reference) */
	ei_x_encode_ref(&gen_reply, tag_ref);

	/* Append the reply data */
	ei_x_append_buf(&gen_reply, x->buff, x->index);

	/* Debug: Print hex dump of reply buffer */
	printf("Test CNode: Reply buffer (hex, first 64 bytes): ");
	for (int i = 0; i < gen_reply.index && i < 64; i++) {
		printf("%02x ", (unsigned char)gen_reply.buff[i]);
	}
	printf("\n");

	/* Send the reply */
	int send_result = ei_send(fd, to_pid, gen_reply.buff, gen_reply.index);
	if (send_result < 0) {
		fprintf(stderr, "Error sending reply (errno: %d, %s)\n", errno, strerror(errno));
	} else {
		printf("Test CNode: Reply sent successfully (%d bytes)\n", gen_reply.index);
		fflush(stdout);
	}

	ei_x_free(&gen_reply);
}

/*
 * Main loop - accept connections and process messages
 */
static void main_loop(void) {
	ei_x_buff x;
	erlang_msg msg;
	int fd;
	int res;

	printf("Test CNode: Entering main loop\n");
	fflush(stdout);

	ei_x_new(&x);

	if (listen_fd < 0) {
		fprintf(stderr, "Test CNode: Invalid listen_fd: %d\n", listen_fd);
		fflush(stderr);
		return;
	}

	printf("Test CNode: Waiting for connections on fd: %d\n", listen_fd);
	fflush(stdout);

	while (1) {
		if (listen_fd < 0) {
			printf("Test CNode: listen_fd closed, exiting\n");
			fflush(stdout);
			break;
		}

		/* Accept connection (blocking) - ei_accept handles the Erlang distribution protocol handshake */
		printf("Test CNode: Waiting for connection (blocking ei_accept)...\n");
		fflush(stdout);
		ErlConnect con;
		fd = ei_accept(&ec, listen_fd, &con);

		if (fd < 0) {
			int saved_errno = errno;
			if (saved_errno == EBADF || saved_errno == 9) {
				fprintf(stderr, "Test CNode: listen_fd closed, exiting\n");
				break;
			} else if (saved_errno == ECONNABORTED || saved_errno == 53) {
				printf("Test CNode: Connection aborted, retrying...\n");
				continue;
			} else if (saved_errno == EINTR) {
				printf("Test CNode: Accept interrupted, retrying...\n");
				continue;
			} else {
				fprintf(stderr, "Test CNode: ei_accept() error (errno: %d, %s), retrying...\n", saved_errno, strerror(saved_errno));
				usleep(100000); // 100ms
				continue;
			}
		}

		printf("Test CNode: ✓ Accepted connection on fd: %d\n", fd);
		if (con.nodename[0] != '\0') {
			printf("Test CNode: Connected from node: %s\n", con.nodename);
		}

		/* Register global name "test_server" on first connection */
		static int name_registered = 0;
		if (!name_registered) {
			erlang_pid *self_pid = ei_self(&ec);
			if (self_pid != NULL && ei_global_register(fd, "test_server", self_pid) == 0) {
				printf("Test CNode: ✓ Registered global name 'test_server'\n");
				name_registered = 1;
			} else {
				fprintf(stderr, "Test CNode: Warning: Failed to register global name 'test_server' (errno: %d, %s)\n", errno, strerror(errno));
			}
		}

		/* Process messages from this connection */
		while (1) {
			/* Wait for data to be available using select() */
			fd_set recv_fds;
			struct timeval recv_timeout;
			FD_ZERO(&recv_fds);
			FD_SET(fd, &recv_fds);
			recv_timeout.tv_sec = 5; /* 5 second timeout */
			recv_timeout.tv_usec = 0;
			int select_res = select(fd + 1, &recv_fds, NULL, NULL, &recv_timeout);

			if (select_res <= 0 || !FD_ISSET(fd, &recv_fds)) {
				/* Timeout or error - check if connection is still alive */
				if (select_res == 0) {
					/* Timeout - connection might be idle, continue waiting */
					continue;
				} else {
					/* select() error - connection might be closed */
					printf("Test CNode: select() error, closing connection\n");
					break;
				}
			}

			/* Data is available, try to receive */
			res = ei_receive_msg(fd, &msg, &x);

			if (res == ERL_TICK) {
				/* Just a tick, continue */
				continue;
			} else if (res == ERL_ERROR) {
				/* Error or connection closed */
				int saved_errno = errno;
				if (saved_errno == ECONNRESET || saved_errno == EPIPE || saved_errno == 0) {
					/* Connection closed or no data (errno 0 can mean EOF) */
					if (saved_errno == 0) {
						/* Check if socket is actually closed by trying to read */
						char test_buf[1];
						ssize_t test_read = recv(fd, test_buf, 1, MSG_PEEK | MSG_DONTWAIT);
						if (test_read == 0) {
							printf("Test CNode: Connection closed by peer (EOF)\n");
							break;
						} else if (test_read < 0 && (errno == ECONNRESET || errno == EPIPE)) {
							printf("Test CNode: Connection closed by peer\n");
							break;
						}
						/* errno 0 but socket still open - might be a false positive, continue */
						continue;
					}
					printf("Test CNode: Connection closed by peer\n");
					break;
				} else {
					fprintf(stderr, "Test CNode: ei_receive_msg error (errno: %d, %s)\n", saved_errno, strerror(saved_errno));
					break;
				}
			} else if (res == ERL_MSG) {
				/* Message received */
				x.index = 0;
				int process_result = process_message(x.buff, &x.index, fd);

				ei_x_free(&x);
				ei_x_new(&x);

				if (process_result < 0) {
					fprintf(stderr, "Test CNode: Error processing message, closing connection\n");
					break;
				}
			}
		}

		/* Close connection */
		close(fd);
		printf("Test CNode: Connection closed\n");
	}

	ei_x_free(&x);
}

/*
 * Main entry point
 */
int main(int argc, char *argv[]) {
	const char *nodename = "test_cnode@127.0.0.1";
	const char *cookie = "godotcookie";

	if (argc > 1) {
		nodename = argv[1];
	}
	if (argc > 2) {
		cookie = argv[2];
	}

	printf("=== Standalone C CNode with GenServer Features ===\n");
	printf("Nodename: %s\n", nodename);
	printf("Cookie: %s\n", cookie);
	printf("\n");

	if (init_cnode(nodename, cookie) < 0) {
		fprintf(stderr, "Failed to initialize CNode\n");
		fflush(stderr);
		return 1;
	}

	printf("Test CNode: Initialization complete, starting main loop...\n");
	printf("Press Ctrl+C to exit\n\n");
	fflush(stdout);

	main_loop();

	printf("Test CNode: Main loop exited\n");
	fflush(stdout);

	/* Cleanup */
	if (listen_fd >= 0) {
		close(listen_fd);
	}
	if (publish_fd >= 0) {
		close(publish_fd);
	}

	return 0;
}
