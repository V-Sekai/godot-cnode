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

// Define POSIX feature test macros BEFORE any includes
// Only on Unix systems (not Windows)
#ifndef _WIN32
#define _POSIX_C_SOURCE 200112L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>

// POSIX-specific headers (not available on Windows)
#ifndef _WIN32
#include <unistd.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <fcntl.h>
#else
// Windows equivalents
#include <io.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <basetsd.h>  // For SSIZE_T
#define close closesocket
#define usleep(x) Sleep((x)/1000)
// Define ssize_t for MSVC (POSIX type not available in MSVC)
#ifndef _SSIZE_T_DEFINED
#define _SSIZE_T_DEFINED
typedef SSIZE_T ssize_t;
#endif
#endif

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
#ifndef _WIN32
	int flags = fcntl(fd, F_GETFL, 0);
	if (flags >= 0) {
		fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
	}
#else
	/* On Windows, use ioctlsocket to set blocking mode */
	unsigned long mode = 0;  // 0 = blocking
	ioctlsocket(fd, FIONBIO, &mode);
#endif

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

	/* Debug output removed - uncomment if needed for debugging */
	/* printf("Test CNode: process_message - starting at index: %d\n", *index); */

	/* Decode version (optional) */
	if (ei_decode_version(buf, index, &version) < 0) {
		printf("Test CNode: No version header, skipping\n");
		*index = saved_index;
	} else {
		/* Version decoded successfully */
		saved_index = *index;
	}

	/* Decode tuple header */
	if (ei_decode_tuple_header(buf, index, &arity) < 0) {
		fprintf(stderr, "Error decoding tuple header at index: %d\n", *index);
		fprintf(stderr, "Buffer bytes at index: ");
		for (int i = 0; i < 16 && i < 1024; i++) {
			fprintf(stderr, "%02x ", (unsigned char)buf[*index + i]);
		}
		fprintf(stderr, "\n");
		fflush(stderr);
		return -1;
	}
	/* Check if this is a GenServer-style message */
	int tuple_start_index = *index;

	if (ei_decode_atom(buf, index, atom) < 0) {
		fprintf(stderr, "Test CNode: Error decoding atom at index: %d\n", *index);
		fprintf(stderr, "Test CNode: Bytes at error position: ");
		for (int i = 0; i < 16 && (*index + i) < 1024; i++) {
			fprintf(stderr, "%02x ", (unsigned char)buf[*index + i]);
		}
		fprintf(stderr, "\n");
		fflush(stderr);
		return -1;
	}
	/* Atom decoded: atom */

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
		printf("Test CNode: From PID: %s, Tag ref: [%d, %d, %d]\n",
			from_pid.node, tag_ref.len, tag_ref.n[0], tag_ref.n[1]);
		printf("Test CNode: Calling handle_call with index: %d\n", *index);
		fflush(stdout);
		int result = handle_call(buf, index, fd, &from_pid, &tag_ref);
		printf("Test CNode: handle_call returned: %d\n", result);
		fflush(stdout);
		return result;
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
		fflush(stdout);

		/* The From field can be a PID or an atom (node name) - skip it, we only need the Request */
		/* Try to decode as PID first, then as atom if that fails */
		int saved_index = *index;
		erlang_pid rpc_from_pid;
		if (ei_decode_pid(buf, index, &rpc_from_pid) < 0) {
			/* Not a PID - try atom (node name) */
			*index = saved_index;
			char from_atom[MAXATOMLEN];
			if (ei_decode_atom(buf, index, from_atom) < 0) {
				fprintf(stderr, "Error decoding From field in rex message (tried PID and atom)\n");
				fflush(stderr);
				return -1;
			}
			printf("Test CNode: rex From field is atom (node name): %s\n", from_atom);
			fflush(stdout);
		} else {
			printf("Test CNode: rex From field is PID: %s\n", rpc_from_pid.node);
			fflush(stdout);
		}

		/* The Request in rex format is the gen_call tuple directly: {'$gen_call', {From, Tag}, Request} */
		int request_arity;
		if (ei_decode_tuple_header(buf, index, &request_arity) < 0) {
			fprintf(stderr, "Error decoding Request tuple in rex message\n");
			fflush(stderr);
			return -1;
		}

		/* The first element should be '$gen_call' */
		char gen_call_atom[MAXATOMLEN];
		if (ei_decode_atom(buf, index, gen_call_atom) < 0 || strcmp(gen_call_atom, "$gen_call") != 0) {
			fprintf(stderr, "Error: Request in rex message is not a gen_call (got: %s)\n", gen_call_atom);
			fflush(stderr);
			return -1;
		}

		/* Decode the {From, Tag} tuple from the gen_call */
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

		printf("Test CNode: Processing rex GenServer call (From PID: %s)\n", from_pid.node);
		printf("Test CNode: Calling handle_call with index: %d\n", *index);
		fflush(stdout);
		int result = handle_call(buf, index, fd, &from_pid, &tag_ref);
		printf("Test CNode: handle_call returned: %d\n", result);
		fflush(stdout);
		return result;
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
			printf("Test CNode: Encoding reply - pong\n");
			fflush(stdout);
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
	printf("Test CNode: About to send reply (reply buffer size: %d bytes)\n", reply.index);
	fflush(stdout);
	send_reply(&reply, fd, from_pid, tag_ref);
	printf("Test CNode: Reply sent, freeing buffer\n");
	fflush(stdout);
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

	/* Send the reply */
	int send_result = ei_send(fd, to_pid, gen_reply.buff, gen_reply.index);
	if (send_result < 0) {
		fprintf(stderr, "Test CNode: Error sending reply (errno: %d, %s)\n", errno, strerror(errno));
#ifdef _WIN32
		int wsa_err = WSAGetLastError();
		fprintf(stderr, "Test CNode: WSA error: %d\n", wsa_err);
#endif
		fflush(stderr);
	} else {
		printf("Test CNode: Reply sent successfully (%d bytes)\n", gen_reply.index);
		fflush(stdout);
		/* Give time for reply transmission */
		usleep(1000000); // 1000ms
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

		/* Track connection with unique ID */
		static int connection_counter = 0;
		int conn_id = ++connection_counter;
		printf("Test CNode: ✓ Accepted connection #%d on fd: %d\n", conn_id, fd);
		if (con.nodename[0] != '\0') {
			printf("Test CNode: [Conn #%d] Connected from node: %s\n", conn_id, con.nodename);
		}
		fflush(stdout);

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
		/* Keep connection open to receive multiple messages and send replies */
		while (1) {
			/* Wait for data to be available using select() */
			fd_set recv_fds;
			struct timeval recv_timeout;
			FD_ZERO(&recv_fds);
			FD_SET(fd, &recv_fds);
			recv_timeout.tv_sec = 5; /* 5 second timeout */
			recv_timeout.tv_usec = 0;
			int select_res = select(fd + 1, &recv_fds, NULL, NULL, &recv_timeout);

			/* select() result: select_res */

			if (select_res <= 0 || !FD_ISSET(fd, &recv_fds)) {
				/* Timeout or error - check if connection is still alive */
				if (select_res == 0) {
					/* Timeout - connection might be idle, continue waiting */
					printf("Test CNode: [Conn #%d] select() timeout, continuing to wait\n", conn_id);
					fflush(stdout);
					continue;
				} else {
					/* select() error - connection might be closed */
					printf("Test CNode: [Conn #%d] select() error (errno: %d, %s), closing connection\n", conn_id, errno, strerror(errno));
					fflush(stdout);
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

				/* Handle EAGAIN/EWOULDBLOCK (errno 35 on macOS) - data not ready yet, retry */
				if (saved_errno == EAGAIN || saved_errno == EWOULDBLOCK || saved_errno == 35) {
					printf("Test CNode: [Conn #%d] ei_receive_msg: data not ready yet (errno: %d), retrying...\n", conn_id, saved_errno);
					fflush(stdout);
					usleep(10000); // 10ms - short delay before retry
					continue;
				}

				/* errno 0 can mean EOF, but also can mean ei_receive_msg couldn't decode the message */
				/* Try to process message from buffer if it has data (similar to macOS compatibility fix) */
				if (saved_errno == 0) {

					/* Check if buffer has any data at all */
					/* Even if index is 0, the buffer might have been allocated and contain data */
					/* Try to peek at the buffer to see if there's data */
					if (x.index > 0 || (x.buff != NULL && x.buffsz > 0)) {
						/* Check if buffer actually contains BERT data (starts with 0x83) */
						int has_bert_data = 0;
						if (x.index > 0 && (unsigned char)x.buff[0] == 0x83) {
							has_bert_data = 1;
						} else if (x.buffsz > 0 && (unsigned char)x.buff[0] == 0x83) {
							has_bert_data = 1;
							x.index = x.buffsz; /* Use full buffer size */
						}

						if (has_bert_data) {
							/* Process the message from buffer */
							x.index = 0;
							int process_result = process_message(x.buff, &x.index, fd);
							ei_x_free(&x);
							ei_x_new(&x);
							if (process_result < 0) {
								fprintf(stderr, "Test CNode: [Conn #%d] Error processing message from buffer\n", conn_id);
								fflush(stderr);
							} else {
								if (process_result == 0) {
									usleep(1000000); // 1000ms - wait for reply transmission
								}
							}
							continue;
						}
					}

					/* Buffer is empty or doesn't contain BERT data - try raw read */
					{
						/* Buffer is empty but ei_receive_msg failed - try raw read */
						/* This can happen when ei_receive_msg can't decode the message format */
						unsigned char raw_buf[4096];
						ssize_t bytes_read = recv(fd, raw_buf, sizeof(raw_buf), 0);

						if (bytes_read > 0) {

							/* Copy raw data to buffer and try to decode */
							ei_x_free(&x);
							ei_x_new(&x);
							memcpy(x.buff, raw_buf, bytes_read);
							x.index = bytes_read;

							/* Try to process the message */
							/* The distribution protocol format is: [4-byte length] [message data] */
							/* The message data contains: "To Name" + actual BERT message(s) */
							/* We need to find the BERT version byte (0x83) in the payload */
							int decode_index = 0;
							if (bytes_read >= 4) {
								/* Check if first 4 bytes are a length header */
								unsigned long msg_len = (raw_buf[0] << 24) | (raw_buf[1] << 16) | (raw_buf[2] << 8) | raw_buf[3];

								if (msg_len > 0 && msg_len < 1048576) { /* Reasonable message size (1MB max) */
									/* Look for ALL BERT version bytes (0x83) in the payload */
									/* The first one is usually in the "To Name" control message */
									/* The actual message comes after that */
									int bert_starts[10]; /* Max 10 potential messages */
									int bert_count = 0;
									for (int i = 4; i < bytes_read && bert_count < 10; i++) {
										if (raw_buf[i] == 0x83) {
											/* Verify this looks like a valid BERT message start */
											/* Next byte should be a valid BERT type (tuple, list, etc.) */
											if (i + 1 < bytes_read) {
												unsigned char next_byte = raw_buf[i + 1];
												/* Valid BERT types after version: 68 (small tuple), 6C (large tuple), etc. */
												if (next_byte == 0x68 || next_byte == 0x6C || next_byte == 0x6A || next_byte == 0x6B) {
													bert_starts[bert_count++] = i;
												}
											}
										}
									}

									if (bert_count > 0) {
										/* Use the last BERT message found (usually the actual message, after "To Name") */
										/* Or if only one, use it */
										int msg_idx = (bert_count > 1) ? bert_count - 1 : 0;
										decode_index = bert_starts[msg_idx];
									} else {
										/* Fallback: try starting at offset 4 */
										decode_index = 4;
									}
								}
							}

							x.index = decode_index;
							int process_result = process_message(x.buff, &x.index, fd);
							ei_x_free(&x);
							ei_x_new(&x);
							if (process_result < 0) {
								fprintf(stderr, "Test CNode: [Conn #%d] Error processing message from raw read\n", conn_id);
								fflush(stderr);
							} else {
								if (process_result == 0) {
									/* Message processed successfully - give time for reply to be sent */
									usleep(1000000); // 1000ms - wait for reply transmission
								}
							}
							/* Continue waiting for more messages on this connection */
							/* Don't break - keep connection open */
							/* Reset buffer for next message */
							ei_x_free(&x);
							ei_x_new(&x);
							continue;
						} else if (bytes_read == 0) {
							/* EOF - connection closed */
							break;
						} else {
							/* Error on raw read */
#ifdef _WIN32
							int wsa_err = WSAGetLastError();
							if (wsa_err == WSAECONNRESET || wsa_err == WSAENOTCONN) {
#else
							if (errno == ECONNRESET || errno == EPIPE) {
#endif
								break;
							}
							/* Other error, continue to check socket state */
						}
					}
				}
				if (saved_errno == ECONNRESET || saved_errno == EPIPE || saved_errno == 0) {
					/* Connection closed or no data (errno 0 can mean EOF) */
					if (saved_errno == 0) {
						/* Check if socket is actually closed by trying to read */
						printf("Test CNode: [Conn #%d] Checking socket state with MSG_PEEK...\n", conn_id);
						fflush(stdout);
						char test_buf[1];
#ifndef _WIN32
						ssize_t test_read = recv(fd, test_buf, 1, MSG_PEEK | MSG_DONTWAIT);
#else
						/* On Windows, use non-blocking recv with timeout */
						unsigned long mode = 1;  // 1 = non-blocking
						ioctlsocket(fd, FIONBIO, &mode);
						ssize_t test_read = recv(fd, test_buf, 1, MSG_PEEK);
						mode = 0;  // Set back to blocking
						ioctlsocket(fd, FIONBIO, &mode);
#endif
						printf("Test CNode: [Conn #%d] MSG_PEEK returned: %zd", conn_id, test_read);
						if (test_read < 0) {
#ifdef _WIN32
							int wsa_err = WSAGetLastError();
							printf(" (WSA error: %d)", wsa_err);
#else
							printf(" (errno: %d, %s)", errno, strerror(errno));
#endif
						}
						printf("\n");
						fflush(stdout);

						if (test_read == 0) {
							printf("Test CNode: [Conn #%d] Connection closed by peer (EOF)\n", conn_id);
							fflush(stdout);
							break;
						} else if (test_read < 0) {
#ifdef _WIN32
							int wsa_err = WSAGetLastError();
							if (wsa_err == WSAECONNRESET || wsa_err == WSAENOTCONN) {
#else
							if (errno == ECONNRESET || errno == EPIPE) {
#endif
								printf("Test CNode: [Conn #%d] Connection closed by peer\n", conn_id);
								fflush(stdout);
								break;
							}
						}
						/* errno 0 but socket still open - might be a false positive, continue */
						printf("Test CNode: [Conn #%d] Socket still open, continuing to wait for data\n", conn_id);
						fflush(stdout);
						continue;
					}
					printf("Test CNode: [Conn #%d] Connection closed by peer (errno: %d)\n", conn_id, saved_errno);
					fflush(stdout);
					break;
				} else {
					fprintf(stderr, "Test CNode: [Conn #%d] ei_receive_msg error (errno: %d, %s)\n", conn_id, saved_errno, strerror(saved_errno));
					fflush(stderr);
					break;
				}
				} else if (res == ERL_MSG) {
				/* Message received */
				/* Save buffer size before resetting index */
				int saved_buffer_index = x.index;

				/* Debug: Check buffer state */
				printf("Test CNode: [Conn #%d] ERL_MSG received: buffer index=%d, buff=%p, first byte=0x%02x\n",
					conn_id, saved_buffer_index, (void*)x.buff,
					(saved_buffer_index > 0 && x.buff != NULL) ? (unsigned char)x.buff[0] : 0);
				fflush(stdout);

				/* Check if this looks like a BERT message (starts with 0x83) */
				/* IMPORTANT: Check BEFORE resetting index */
				/* CRITICAL: Cast to unsigned char to avoid sign extension issues */
				int is_bert = (saved_buffer_index > 0 && x.buff != NULL && (unsigned char)x.buff[0] == 0x83);
				printf("Test CNode: [Conn #%d] BERT check: saved_index=%d, buff!=NULL=%d, first_byte=0x%02x, is_bert=%d\n",
					conn_id, saved_buffer_index, (x.buff != NULL ? 1 : 0),
					(saved_buffer_index > 0 && x.buff != NULL) ? (unsigned char)x.buff[0] : 0, is_bert);
				fflush(stdout);

				if (is_bert) {
					printf("Test CNode: [Conn #%d] Message is BERT format, processing...\n", conn_id);
					fflush(stdout);
					x.index = 0;
					int process_result = process_message(x.buff, &x.index, fd);

					ei_x_free(&x);
					ei_x_new(&x);

					if (process_result < 0) {
						fprintf(stderr, "Test CNode: [Conn #%d] Error processing message, closing connection\n", conn_id);
						fflush(stderr);
						break;
					}

					/* For GenServer calls, give time for reply to be sent and received */
					/* Don't close connection immediately - keep it open for potential replies */
					if (process_result == 0) {
						/* Message processed successfully - wait a bit for reply transmission */
						usleep(100000); // 100ms
					}
				} else {
					/* Not a BERT message - might be a system message */
					/* Free and continue - this might be a system message we don't handle */
					/* But also check if there's more data available - the actual message might come next */
					ei_x_free(&x);
					ei_x_new(&x);
					/* Continue to wait for the actual BERT message */
					continue;
				}
			}
		}

		/* Close connection */
		close(fd);
	}

	ei_x_free(&x);
}

/*
 * Main entry point
 */
int main(int argc, char *argv[]) {
#ifdef _WIN32
	// Initialize Winsock on Windows
	WSADATA wsaData;
	int wsaResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
	if (wsaResult != 0) {
		fprintf(stderr, "WSAStartup failed: %d\n", wsaResult);
		return 1;
	}
#endif

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

#ifdef _WIN32
	// Cleanup Winsock on Windows
	WSACleanup();
#endif

	return 0;
}
