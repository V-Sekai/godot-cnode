/*
 * Godot CNode - Erlang/Elixir CNode interface for Godot Engine (GDExtension)
 *
 * This CNode allows Elixir/Erlang nodes to communicate with Godot
 * using the Erlang distribution protocol.
 *
 * Adapted for GDExtension - works with current Godot instance
 */

// Define POSIX feature test macros BEFORE any includes to ensure timespec is defined
// Only on Unix systems (not Windows)
#ifndef _WIN32
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif
#define _POSIX_C_SOURCE 200112L
#endif

// Include C headers that define timespec BEFORE any C++ headers
#include <errno.h>
#include <time.h>
#include <cstdio>
#include <cstring>

// POSIX-specific headers (not available on Windows)
#ifndef _WIN32
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#else
// Windows equivalents
#include <io.h>
#include <windows.h>
#define close _close
#endif

// erl_interface headers
extern "C" {
#include "ei.h"
#include "ei_connect.h"
}

/* Forward declaration - ei_default_socket_callbacks is not in public headers */
/* We'll access it via the ei_cnode structure after initialization */
extern "C" {
extern ei_socket_callbacks ei_default_socket_callbacks;
}

// Godot-cpp includes
#include <godot_cpp/classes/class_db_singleton.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/main_loop.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/window.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/godot.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/node_path.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/variant/variant.hpp>

#include "godot_cnode.h"

using namespace godot;

/* Get SceneTree singleton using godot-cpp */
static SceneTree *get_scene_tree() {
	Engine *engine = Engine::get_singleton();
	if (engine == nullptr) {
		return nullptr;
	}
	MainLoop *main_loop = engine->get_main_loop();
	if (main_loop == nullptr) {
		return nullptr;
	}
	return Object::cast_to<SceneTree>(main_loop);
}

/* Get root node from scene tree */
static Node *get_scene_tree_root(SceneTree *tree) {
	if (tree == nullptr)
		return nullptr;
	return tree->get_current_scene();
}

/* Find node by path */
static Node *find_node_by_path(SceneTree *tree, const char *path_str) {
	if (tree == nullptr || path_str == nullptr)
		return nullptr;
	Node *root = tree->get_current_scene();
	if (root == nullptr)
		return nullptr;
	NodePath path = NodePath(String::utf8(path_str));
	return root->get_node_or_null(path);
}

/* Get node name as string */
static const char *get_node_name(Node *node) {
	if (node == nullptr)
		return nullptr;
	String name = node->get_name();
	return name.utf8().get_data();
}

/* Get node by instance ID using godot-cpp */
static Node *get_node_by_id(int64_t node_id) {
	if (node_id == 0)
		return nullptr;
	// In godot-cpp, we use ObjectDB::get_instance()
	ObjectID obj_id = ObjectID((uint64_t)node_id);
	Object *obj = ObjectDB::get_instance(obj_id);
	if (obj == nullptr)
		return nullptr;
	return Object::cast_to<Node>(obj);
}

/* Convert BERT to Variant (decode from ei buffer) */
/* skip_version: if true, skip version decoding (used when already inside a tuple/list) */
static Variant bert_to_variant(char *buf, int *index, bool skip_version = false) {
	int type, arity;
	char atom[MAXATOMLEN];
	long long_val;
	double double_val;
	char string_buf[256];

	if (!skip_version) {
		if (ei_decode_version(buf, index, NULL) < 0) {
			return Variant(); // Error
		}
	}

	if (ei_get_type(buf, index, &type, &arity) < 0) {
		return Variant(); // Error
	}

	switch (type) {
		case ERL_ATOM_EXT:
			if (ei_decode_atom(buf, index, atom) == 0) {
				if (strcmp(atom, "nil") == 0) {
					return Variant();
				} else if (strcmp(atom, "true") == 0) {
					return Variant(true);
				} else if (strcmp(atom, "false") == 0) {
					return Variant(false);
				}
			}
			return Variant(String::utf8(atom));

		case ERL_INTEGER_EXT:
			if (ei_decode_long(buf, index, &long_val) == 0) {
				return Variant((int64_t)long_val);
			}
			break;

		case ERL_FLOAT_EXT:
		case NEW_FLOAT_EXT:
			if (ei_decode_double(buf, index, &double_val) == 0) {
				return Variant(double_val);
			}
			break;

		case ERL_STRING_EXT:
			if (ei_decode_string(buf, index, string_buf) == 0) {
				return Variant(String::utf8(string_buf));
			}
			break;

		case ERL_LIST_EXT:
			// Decode list header to get actual arity
			if (ei_decode_list_header(buf, index, &arity) < 0) {
				return Variant(); // Error
			}
			if (arity == 0) {
				// Empty list - check for tail (should be nil)
				int tail_type, tail_size;
				if (ei_get_type(buf, index, &tail_type, &tail_size) == 0 && tail_type == ERL_NIL_EXT) {
					ei_skip_term(buf, index);
				}
				return Variant(Array());
			} else {
				Array arr;
				for (int i = 0; i < arity; i++) {
					Variant elem = bert_to_variant(buf, index, true); // Skip version, already in list
					arr.push_back(elem);
				}
				// Check and skip the list tail (should be nil/empty list)
				int tail_type, tail_size;
				if (ei_get_type(buf, index, &tail_type, &tail_size) == 0 && tail_type == ERL_NIL_EXT) {
					ei_skip_term(buf, index);
				}
				return Variant(arr);
			}

		case ERL_SMALL_TUPLE_EXT:
		case ERL_LARGE_TUPLE_EXT:
			if (ei_decode_tuple_header(buf, index, &arity) == 0 && arity > 0) {
				if (ei_decode_atom(buf, index, atom) == 0) {
					if (strcmp(atom, "vector2") == 0 && arity == 3) {
						double x, y;
						ei_decode_double(buf, index, &x);
						ei_decode_double(buf, index, &y);
						return Variant(Vector2(x, y));
					} else if (strcmp(atom, "vector3") == 0 && arity == 4) {
						double x, y, z;
						ei_decode_double(buf, index, &x);
						ei_decode_double(buf, index, &y);
						ei_decode_double(buf, index, &z);
						return Variant(Vector3(x, y, z));
					} else if (strcmp(atom, "color") == 0 && arity == 5) {
						double r, g, b, a;
						ei_decode_double(buf, index, &r);
						ei_decode_double(buf, index, &g);
						ei_decode_double(buf, index, &b);
						ei_decode_double(buf, index, &a);
						return Variant(Color(r, g, b, a));
					} else if (strcmp(atom, "dictionary") == 0 && arity == 2) {
						Dictionary dict;
						long dict_size;
						ei_decode_long(buf, index, &dict_size);
						for (long i = 0; i < dict_size; i++) {
							Variant key = bert_to_variant(buf, index, true); // Skip version, already in tuple
							Variant value = bert_to_variant(buf, index, true); // Skip version, already in tuple
							dict[key] = value;
						}
						return Variant(dict);
					}
				}
			}
			break;

		case ERL_NIL_EXT:
			// Empty list [] can be encoded as ERL_NIL_EXT
			ei_skip_term(buf, index);
			return Variant(); // Return NIL - caller should handle conversion to empty array if needed
	}

	return Variant(); // Error or unsupported type
}

/* Convert Variant to BERT (encode to ei buffer) */
static void variant_to_bert(const Variant &var, ei_x_buff *x) {
	Variant::Type type = var.get_type();

	switch (type) {
		case Variant::NIL:
			ei_x_encode_atom(x, "nil");
			break;

		case Variant::BOOL:
			ei_x_encode_atom(x, var.operator bool() ? "true" : "false");
			break;

		case Variant::INT:
			ei_x_encode_long(x, var.operator int64_t());
			break;

		case Variant::FLOAT:
			ei_x_encode_double(x, var.operator double());
			break;

		case Variant::STRING: {
			String str = var.operator String();
			ei_x_encode_string(x, str.utf8().get_data());
			break;
		}

		case Variant::VECTOR2: {
			Vector2 vec = var.operator Vector2();
			ei_x_encode_tuple_header(x, 3);
			ei_x_encode_atom(x, "vector2");
			ei_x_encode_double(x, vec.x);
			ei_x_encode_double(x, vec.y);
			break;
		}

		case Variant::VECTOR3: {
			Vector3 vec = var.operator Vector3();
			ei_x_encode_tuple_header(x, 4);
			ei_x_encode_atom(x, "vector3");
			ei_x_encode_double(x, vec.x);
			ei_x_encode_double(x, vec.y);
			ei_x_encode_double(x, vec.z);
			break;
		}

		case Variant::COLOR: {
			Color col = var.operator Color();
			ei_x_encode_tuple_header(x, 5);
			ei_x_encode_atom(x, "color");
			ei_x_encode_double(x, col.r);
			ei_x_encode_double(x, col.g);
			ei_x_encode_double(x, col.b);
			ei_x_encode_double(x, col.a);
			break;
		}

		case Variant::ARRAY: {
			Array arr = var.operator Array();
			ei_x_encode_list_header(x, arr.size());
			for (int i = 0; i < arr.size(); i++) {
				variant_to_bert(arr[i], x);
			}
			ei_x_encode_empty_list(x);
			break;
		}

		case Variant::DICTIONARY: {
			Dictionary dict = var.operator Dictionary();
			Array keys = dict.keys();
			ei_x_encode_map_header(x, keys.size());
			for (int i = 0; i < keys.size(); i++) {
				Variant key = keys[i];
				Variant value = dict[key];
				variant_to_bert(key, x);
				variant_to_bert(value, x);
			}
			break;
		}

		case Variant::OBJECT: {
			Object *obj = var.operator Object *();
			if (obj == nullptr) {
				ei_x_encode_atom(x, "nil");
			} else {
				// Encode object as tuple with type name and instance ID
				ei_x_encode_tuple_header(x, 3);
				ei_x_encode_atom(x, "object");
				String class_name = obj->get_class();
				ei_x_encode_string(x, class_name.utf8().get_data());
				ei_x_encode_long(x, (int64_t)obj->get_instance_id());
			}
			break;
		}

		default: {
			// For unsupported types, encode as tuple with type name
			ei_x_encode_tuple_header(x, 2);
			ei_x_encode_atom(x, "unsupported");
			String type_name = Variant::get_type_name(type);
			ei_x_encode_string(x, type_name.utf8().get_data());
			break;
		}
	}
}

/* CNode configuration */
#define MAXBUFLEN 8192
/* MAXATOMLEN is already defined in ei.h */

/* Global state */
ei_cnode ec;
extern "C" {
int listen_fd = -1;
godot_instance_t instances[MAX_INSTANCES];
}
int next_instance_id = 1;

/* Forward declarations */
static int process_message(char *buf, int *index, int fd);
static int handle_call(char *buf, int *index, int fd, erlang_pid *from_pid, erlang_ref *tag_ref);
static int handle_cast(char *buf, int *index);
static void send_reply(ei_x_buff *x, int fd, erlang_pid *to_pid, erlang_ref *tag_ref);

/* Custom socket callbacks for macOS compatibility */
/* macOS doesn't support SO_ACCEPTCONN, so we need a custom accept implementation */

/* Helper to extract FD from context (same as default implementation) */
#define EI_DFLT_CTX_TO_FD__(CTX, FD)      \
	((intptr_t)(CTX) < 0                  \
					? (*(FD) = -1, EBADF) \
					: (*(FD) = (int)(intptr_t)(CTX), 0))

/* Helper to convert FD to context */
#define EI_FD_AS_CTX__(FD) ((void *)(intptr_t)(FD))

/* macOS-compatible accept callback */
/* This is allowed to call accept() directly - it's the intended use in callbacks */
static int macos_tcp_accept(void **ctx, void *addr, int *len, unsigned unused) {
	int fd, res;
	socklen_t addr_len = (socklen_t)*len;

	if (!ctx)
		return EINVAL;

	/* Extract file descriptor from context */
	res = EI_DFLT_CTX_TO_FD__(*ctx, &fd);
	if (res)
		return res;

	/* Ensure socket is in blocking mode (not non-blocking) */
	/* This helps avoid macOS-specific issues */
	int flags = fcntl(fd, F_GETFL, 0);
	if (flags >= 0) {
		fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
	}

	/* Call accept() directly - this is allowed in callbacks */
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
/* ei_default_socket_callbacks is declared as extern in the library */

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
/* Most callbacks delegate to default implementations, only accept is custom */
static ei_socket_callbacks custom_socket_callbacks = {
	0, /* flags */
	custom_socket,
	custom_close,
	custom_listen,
	macos_tcp_accept, /* accept - custom macOS-compatible implementation */
	custom_connect,
#ifdef __APPLE__
/* On macOS, check if writev is available */
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
extern "C" {
int init_cnode(char *nodename, char *cookie) {
	int res;
	int fd;

	/* Zero-initialize the ei_cnode structure */
	memset(&ec, 0, sizeof(ei_cnode));

	/* Validate inputs */
	if (nodename == nullptr || strlen(nodename) == 0) {
		fprintf(stderr, "ei_connect_init: invalid nodename (null or empty)\n");
		return -1;
	}

	/* Validate nodename format: must contain @ */
	if (strchr(nodename, '@') == nullptr) {
		fprintf(stderr, "ei_connect_init: invalid nodename format (must be 'name@hostname'): %s\n", nodename);
		return -1;
	}

	/* Validate nodename length - Erlang has limits on atom length */
	if (strlen(nodename) > 256) {
		fprintf(stderr, "ei_connect_init: nodename too long (max 256 characters): %zu\n", strlen(nodename));
		return -1;
	}

	if (cookie == nullptr || strlen(cookie) == 0) {
		fprintf(stderr, "ei_connect_init: invalid cookie (null or empty)\n");
		return -1;
	}
	if (strlen(cookie) > MAXATOMLEN) {
		fprintf(stderr, "ei_connect_init: cookie too long (max %d characters)\n", MAXATOMLEN);
		return -1;
	}

	/* Initialize ei library */
	/* Note: ei_init() must be called before ei_connect_init() on some systems (especially macOS) */
	ei_init();

	/* Extract hostname and alivename from nodename (format: "name@hostname") */
	char thishostname[EI_MAXHOSTNAMELEN + 1] = { 0 };
	char thisalivename[EI_MAXALIVELEN + 1] = { 0 };
	char thisnodename[MAXNODELEN + 1] = { 0 };

	char *at_pos = strchr(nodename, '@');
	if (at_pos == nullptr) {
		fprintf(stderr, "ei_connect_xinit_ussi: invalid nodename format (must be 'name@hostname'): %s\n", nodename);
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
		fprintf(stderr, "ei_connect_xinit_ussi: hostname too long: %s\n", hostname);
		return -1;
	}
	strncpy(thishostname, hostname, sizeof(thishostname) - 1);
	thishostname[sizeof(thishostname) - 1] = '\0';

	/* Full nodename */
	if (strlen(nodename) >= sizeof(thisnodename)) {
		fprintf(stderr, "ei_connect_xinit_ussi: nodename too long: %s\n", nodename);
		return -1;
	}
	strncpy(thisnodename, nodename, sizeof(thisnodename) - 1);
	thisnodename[sizeof(thisnodename) - 1] = '\0';

	/* Use ei_connect_xinit_ussi with custom socket callbacks for macOS compatibility */
	/* This allows us to override the accept callback without patching the library */
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
		fprintf(stderr, "  cookie: %s (length: %zu)\n", cookie, strlen(cookie));
		return -1;
	}

	/* Create listening socket with ei_listen (recommended approach) */
	/* ei_listen creates a socket properly configured for Erlang distribution */
	int port = 0; // Let system choose port (will be updated by ei_listen)
	fd = ei_listen(&ec, &port, 5); // backlog of 5
	if (fd < 0) {
		fprintf(stderr, "ei_listen failed: %d (errno: %d, %s)\n", fd, errno, strerror(errno));
		return -1;
	}
	printf("Godot CNode: Created listening socket on port %d\n", port);

	/* Verify socket is in correct state after ei_listen */
	int optval;
	socklen_t optlen = sizeof(optval);
	if (getsockopt(fd, SOL_SOCKET, SO_ACCEPTCONN, &optval, &optlen) == 0) {
		if (optval == 1) {
			printf("Godot CNode: Socket verified: SO_ACCEPTCONN=1 (listening)\n");
		} else {
			fprintf(stderr, "Godot CNode: Warning: Socket SO_ACCEPTCONN=%d (expected 1)\n", optval);
		}
	} else {
		fprintf(stderr, "Godot CNode: Warning: Could not check SO_ACCEPTCONN: %s\n", strerror(errno));
	}

	/* Check for any socket errors */
	int socket_error = 0;
	socklen_t error_len = sizeof(socket_error);
	if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &error_len) == 0) {
		if (socket_error != 0) {
			fprintf(stderr, "Godot CNode: Socket error detected: %d (%s)\n", socket_error, strerror(socket_error));
		}
	}

	/* Now register with epmd using the port from ei_listen */
	/* ei_publish registers the node with epmd so other nodes can discover it */
	int publish_result = ei_publish(&ec, port);
	if (publish_result < 0) {
		fprintf(stderr, "ei_publish failed: %d (errno: %d, %s)\n", publish_result, errno, strerror(errno));
		if (errno == ECONNREFUSED || errno == 61) {
			fprintf(stderr, "  epmd (Erlang Port Mapper Daemon) is not running\n");
			fprintf(stderr, "  To fix: Start epmd with 'epmd -daemon'\n");
			fprintf(stderr, "  Note: Node will still listen on port %d but won't be discoverable via epmd\n", port);
			/* Continue anyway - the socket is still valid for accepting connections */
		} else if (errno == 42 || errno == ENOPROTOOPT) {
			/* macOS issue: Protocol not available - same as SO_ACCEPTCONN issue */
			/* The socket is still valid, continue anyway */
			fprintf(stderr, "  Note: macOS compatibility issue (errno 42), but socket is still valid\n");
			fprintf(stderr, "  Node will still listen on port %d\n", port);
			/* Continue anyway - the socket is still valid for accepting connections */
		} else {
			/* Other error - close the socket and fail */
			close(fd);
			return -1;
		}
	} else {
		printf("Godot CNode: Successfully published node with epmd on port %d\n", port);
	}

	/* Use the ei_listen socket for accepting connections */
	/* ei_publish returns a file descriptor for epmd communication, but we use ei_listen's socket */
	/* This socket is properly configured for ei_accept to handle Erlang distribution protocol */
	listen_fd = fd;

	/* Verify socket is valid and ready */
	if (listen_fd < 0) {
		fprintf(stderr, "Godot CNode: Invalid listen_fd after initialization\n");
		return -1;
	}

	/* BINARY SEARCH: Test 1 - Minimal configuration (no socket options) */
	/* Test if ei_accept() works with socket as-is from ei_listen() */
	printf("Godot CNode: BINARY SEARCH Test 1: No socket options set\n");

	printf("Godot CNode: Socket ready for accepting connections (fd: %d, port: %d)\n", listen_fd, port);
	return 0;
}
} // extern "C"

/*
 * Process incoming message from Erlang/Elixir
 * Handles both GenServer-style messages {call, ...} / {cast, ...}
 * and direct RPC calls from :rpc.call
 */
/*
 * Process incoming message from Erlang/Elixir
 * Follows the Erlang Interface User's Guide format:
 * https://www.erlang.org/doc/apps/erl_interface/ei_users_guide.html#sending-and-receiving-erlang-messages
 *
 * Message format after ei_receive_msg:
 * - Version (optional)
 * - Tuple header
 * - Tuple elements: {Module, Function, Args} for plain RPC calls
 */
static int process_message(char *buf, int *index, int fd) {
	// Guard: Check for null pointers
	if (buf == nullptr || index == nullptr) {
		fprintf(stderr, "Error: null pointer in process_message\n");
		return -1;
	}

	// Guard: Check for valid file descriptor
	if (fd < 0) {
		fprintf(stderr, "Error: invalid file descriptor in process_message\n");
		return -1;
	}

	int version;
	int arity;
	char atom[MAXATOMLEN];
	int saved_index = *index;

	/* Decode version (as per Erlang Interface User's Guide) */
	if (ei_decode_version(buf, index, &version) < 0) {
		/* Some messages may not have version header - try without it */
		*index = saved_index;
	} else {
		saved_index = *index; // Update saved_index after version
	}

	/* Decode tuple header (as per Erlang Interface User's Guide) */
	if (ei_decode_tuple_header(buf, index, &arity) < 0) {
		fprintf(stderr, "Error decoding tuple header\n");
		return -1;
	}

	/* Check if this is a GenServer-style message by peeking at the first atom */
	/* Save current position to restore if it's not GenServer */
	int tuple_start_index = *index;
	if (ei_decode_atom(buf, index, atom) < 0) {
		fprintf(stderr, "Error decoding atom\n");
		return -1;
	}

	/* Handle GenServer-style messages for synchronous RPC calls (with replies) */
	if (strcmp(atom, "$gen_call") == 0) {
		/* GenServer call: {'$gen_call', {From, Tag}, Request} - synchronous with reply */
		// Decode the From tuple {From, Tag}
		int from_arity;
		if (ei_decode_tuple_header(buf, index, &from_arity) < 0 || from_arity != 2) {
			fprintf(stderr, "Error decoding From tuple in gen_call\n");
			return -1;
		}
		// Decode From (PID) and Tag (reference) for sending reply
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
		// Now decode the Request tuple {Module, Function, Args} and handle with reply
		printf("Godot CNode: Received GenServer call (synchronous RPC with reply)\n");
		return handle_call(buf, index, fd, &from_pid, &tag_ref);
	} else if (strcmp(atom, "$gen_cast") == 0) {
		/* GenServer cast: {'$gen_cast', Request} - asynchronous, no reply */
		// Request is directly after the atom, which is {Module, Function, Args}
		return handle_cast(buf, index);
	} else if (strcmp(atom, "rex") == 0) {
		/* RPC message format: {rex, From, Request} where Request is {'$gen_call', {From, Tag}, ...} */
		/* This format is used when sending to registered names via :erlang.send() */
		printf("Godot CNode: Received RPC message (rex format)\n");
		// Decode From (PID) - this is the RPC caller
		erlang_pid rpc_from_pid;
		if (ei_decode_pid(buf, index, &rpc_from_pid) < 0) {
			fprintf(stderr, "Error decoding From PID in rex message\n");
			return -1;
		}
		// The Request is the GenServer call message - decode it
		int request_arity;
		if (ei_decode_tuple_header(buf, index, &request_arity) < 0) {
			fprintf(stderr, "Error decoding Request tuple in rex message\n");
			return -1;
		}
		// Now decode the '$gen_call' atom
		char gen_call_atom[MAXATOMLEN];
		if (ei_decode_atom(buf, index, gen_call_atom) < 0 || strcmp(gen_call_atom, "$gen_call") != 0) {
			fprintf(stderr, "Error: Request in rex message is not a gen_call (got: %s)\n", gen_call_atom);
			return -1;
		}
		// Now process the gen_call part (decode {From, Tag} and Request)
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
		// Now handle the call with the decoded From and Tag
		printf("Godot CNode: Processing rex GenServer call (synchronous RPC with reply)\n");
		return handle_call(buf, index, fd, &from_pid, &tag_ref);
	} else {
		/* Handle plain messages: {Module, Function, Args} - asynchronous, no reply */
		/* Reset to before tuple header (after version) so handle_cast can decode it */
		*index = saved_index;
		printf("Godot CNode: Received plain message (asynchronous, no reply)\n");
		// handle_cast will decode the tuple header and elements
		return handle_cast(buf, index);
	}
}

/*
 * Find instance by ID (in GDExtension, we only have one instance - the current Godot instance)
 */
static godot_instance_t *find_instance(int id) {
	for (int i = 0; i < MAX_INSTANCES; i++) {
		if (instances[i].id == id && instances[i].scene_tree != nullptr) {
			return &instances[i];
		}
	}
	return nullptr;
}

/*
 * Get or create the current instance (GDExtension works with current Godot instance)
 */
static godot_instance_t *get_current_instance() {
	// Find existing instance
	for (int i = 0; i < MAX_INSTANCES; i++) {
		if (instances[i].id != 0 && instances[i].scene_tree != nullptr) {
			return &instances[i];
		}
	}

	// Create new instance entry for current Godot instance
	for (int i = 0; i < MAX_INSTANCES; i++) {
		if (instances[i].id == 0) {
			instances[i].id = next_instance_id++;
			instances[i].scene_tree = get_scene_tree();
			instances[i].started = (instances[i].scene_tree != nullptr) ? 1 : 0;
			return &instances[i];
		}
	}
	return nullptr;
}

/* Helper: Get object by instance ID (generic, not just Node) */
static Object *get_object_by_id(int64_t object_id) {
	if (object_id == 0)
		return nullptr;
	ObjectID obj_id = ObjectID((uint64_t)object_id);
	Object *obj = ObjectDB::get_instance(obj_id);
	return obj;
}

/* Helper: Execute Godot API call using call_deferred() from background thread */
static void execute_godot_call_deferred(int64_t object_id, const String &method_name, const Array &method_args) {
	// WARNING: ObjectDB::get_instance() may not be thread-safe!
	// We need to get the object pointer to call call_deferred() on it
	// But accessing ObjectDB from background thread might crash
	// For now, we'll try it but this might need a different approach

	// WARNING: get_object_by_id() calls ObjectDB::get_instance() which may not be thread-safe
	// This might crash if called from background thread
	Object *obj = get_object_by_id(object_id);

	if (obj == nullptr) {
		printf("Godot CNode: execute_godot_call_deferred - Error: Object not found (ID: %lld)\n", object_id);
		return;
	}

	// Use call_deferred() to queue the method call on the main thread
	// call_deferred() is thread-safe and can be called from any thread
	// We need to unpack the Array arguments to pass to call_deferred()
	// call_deferred() supports up to 5 arguments, so we handle that many

	if (method_args.size() == 0) {
		obj->call_deferred(method_name);
	} else if (method_args.size() == 1) {
		obj->call_deferred(method_name, method_args[0]);
	} else if (method_args.size() == 2) {
		obj->call_deferred(method_name, method_args[0], method_args[1]);
	} else if (method_args.size() == 3) {
		obj->call_deferred(method_name, method_args[0], method_args[1], method_args[2]);
	} else if (method_args.size() == 4) {
		obj->call_deferred(method_name, method_args[0], method_args[1], method_args[2], method_args[3]);
	} else if (method_args.size() == 5) {
		obj->call_deferred(method_name, method_args[0], method_args[1], method_args[2], method_args[3], method_args[4]);
	} else {
		// For more than 5 args, use call_deferred with "callv" method
		// callv() accepts method name and Array of arguments - no limit!
		obj->call_deferred("callv", method_name, method_args);
		printf("Godot CNode: execute_godot_call_deferred - Using callv for %lld args\n", (long long)method_args.size());
	}

	printf("Godot CNode: Queued call_deferred for ObjectID: %lld, Method: %s\n", object_id, method_name.utf8().get_data());
}

/* Helper: Set property using call_deferred() from background thread */
static void execute_godot_set_property_deferred(int64_t object_id, const String &property_name, const Variant &value) {
	// WARNING: ObjectDB::get_instance() may not be thread-safe!
	// We need to get the object pointer to call call_deferred() on it
	// But accessing ObjectDB from background thread might crash

	// WARNING: get_object_by_id() calls ObjectDB::get_instance() which may not be thread-safe
	// This might crash if called from background thread
	Object *obj = get_object_by_id(object_id);

	if (obj == nullptr) {
		printf("Godot CNode: execute_godot_set_property_deferred - Error: Object not found (ID: %lld)\n", object_id);
		return;
	}

	// Use call_deferred() to queue the set() call on the main thread
	// set() is a method on Object that takes property name and value
	obj->call_deferred("set", property_name, value);
	printf("Godot CNode: Queued call_deferred set for ObjectID: %lld, Property: %s\n", object_id, property_name.utf8().get_data());
}

/* Helper: Encode method info to BERT */
static void encode_method_info(const Dictionary &method, ei_x_buff *x) {
	ei_x_encode_tuple_header(x, 4);
	ei_x_encode_string(x, method["name"].operator String().utf8().get_data());

	Dictionary return_val = method["return"];
	int return_type = return_val["type"];
	ei_x_encode_long(x, return_type);

	Array args = method["args"];
	ei_x_encode_list_header(x, args.size());
	for (int i = 0; i < args.size(); i++) {
		Dictionary arg = args[i];
		ei_x_encode_tuple_header(x, 2);
		ei_x_encode_string(x, arg["name"].operator String().utf8().get_data());
		ei_x_encode_long(x, (long)(int64_t)arg["type"]);
	}
	ei_x_encode_empty_list(x);

	ei_x_encode_long(x, (long)(int64_t)method["flags"]);
}

/* Helper: Encode property info to BERT */
static void encode_property_info(const Dictionary &property, ei_x_buff *x) {
	ei_x_encode_tuple_header(x, 3);
	ei_x_encode_string(x, property["name"].operator String().utf8().get_data());
	ei_x_encode_long(x, (long)(int64_t)property["type"]);
	ei_x_encode_string(x, property.get("class_name", "").operator String().utf8().get_data());
}

/*
 * Handle synchronous call from Erlang/Elixir (GenServer-style with reply)
 */
static int handle_call(char *buf, int *index, int fd, erlang_pid *from_pid, erlang_ref *tag_ref) {
	// Guard: Check for null pointers
	if (buf == nullptr || index == nullptr) {
		fprintf(stderr, "Error: null pointer in handle_call\n");
		return -1;
	}

	// Guard: Check for valid file descriptor
	if (fd < 0) {
		fprintf(stderr, "Error: invalid file descriptor in handle_call\n");
		return -1;
	}

	ei_x_buff reply;
	godot_instance_t *inst;

	/* Initialize reply buffer */
	ei_x_new(&reply);

	/* Decode Request: {Module, Function, Args} */
	int request_arity;
	if (ei_decode_tuple_header(buf, index, &request_arity) < 0 || request_arity < 2) {
		printf("Godot CNode: handle_call - Failed to decode Request tuple header (index: %d)\n", *index);
		fflush(stdout);
		ei_x_encode_tuple_header(&reply, 2);
		ei_x_encode_atom(&reply, "error");
		ei_x_encode_string(&reply, "invalid_request_format");
		send_reply(&reply, fd, from_pid, tag_ref);
		ei_x_free(&reply);
		return -1;
	}

	/* Decode Module and Function from Request tuple */
	char module[256];
	char function[256];

	if (ei_decode_atom(buf, index, module) < 0) {
		printf("Godot CNode: handle_call - Failed to decode Module (index: %d)\n", *index);
		fflush(stdout);
		ei_x_encode_tuple_header(&reply, 2);
		ei_x_encode_atom(&reply, "error");
		ei_x_encode_string(&reply, "invalid_module");
		send_reply(&reply, fd, from_pid, tag_ref);
		ei_x_free(&reply);
		return -1;
	}

	if (ei_decode_atom(buf, index, function) < 0) {
		printf("Godot CNode: handle_call - Failed to decode Function (index: %d)\n", *index);
		fflush(stdout);
		ei_x_encode_tuple_header(&reply, 2);
		ei_x_encode_atom(&reply, "error");
		ei_x_encode_string(&reply, "invalid_function");
		send_reply(&reply, fd, from_pid, tag_ref);
		ei_x_free(&reply);
		return -1;
	}

	printf("Godot CNode: handle_call - Decoded Request: Module=%s, Function=%s, Arity=%d\n", module, function, request_arity);
	fflush(stdout);

	// Decode arguments (remaining elements in Request tuple)
	Array args;
	if (request_arity > 2) {
		// Decode args array
		Variant args_variant = bert_to_variant(buf, index, true); // Skip version, already in tuple
		if (args_variant.get_type() == Variant::ARRAY) {
			args = args_variant.operator Array();
			printf("Godot CNode: handle_call - Decoded args array with %lld elements\n", (long long)args.size());
		} else {
			// Single argument, wrap in array
			args.push_back(args_variant);
			printf("Godot CNode: handle_call - Decoded single arg, wrapped in array\n");
		}
		fflush(stdout);
	}

	// Route based on module
	if (strcmp(module, "godot") == 0) {
		// Generic Godot API calls - now safe since we're on main thread
		if (strcmp(function, "call_method") == 0) {
			if (args.size() >= 2) {
				int64_t object_id = args[0].operator int64_t();
				String method_name = args[1].operator String();
				Array method_args;
				if (args.size() > 2 && args[2].get_type() == Variant::ARRAY) {
					method_args = args[2].operator Array();
				}

				// Get object and call method using callv() which supports unlimited arguments
				Object *obj = ObjectDB::get_instance(ObjectID((uint64_t)object_id));
				if (obj != nullptr) {
					Variant result;
					// Use callv() which accepts an Array of arguments - no limit!
					result = obj->callv(method_name, method_args);

					// Encode result
					variant_to_bert(result, &reply);
				} else {
					ei_x_encode_tuple_header(&reply, 2);
					ei_x_encode_atom(&reply, "error");
					ei_x_encode_string(&reply, "object_not_found");
				}
			} else {
				ei_x_encode_tuple_header(&reply, 2);
				ei_x_encode_atom(&reply, "error");
				ei_x_encode_string(&reply, "insufficient_arguments");
			}
		} else if (strcmp(function, "get_property") == 0) {
			if (args.size() >= 2) {
				int64_t object_id = args[0].operator int64_t();
				String prop_name = args[1].operator String();

				Object *obj = ObjectDB::get_instance(ObjectID((uint64_t)object_id));
				if (obj != nullptr) {
					Variant value = obj->get(prop_name);
					variant_to_bert(value, &reply);
				} else {
					ei_x_encode_tuple_header(&reply, 2);
					ei_x_encode_atom(&reply, "error");
					ei_x_encode_string(&reply, "object_not_found");
				}
			} else {
				ei_x_encode_tuple_header(&reply, 2);
				ei_x_encode_atom(&reply, "error");
				ei_x_encode_string(&reply, "insufficient_arguments");
			}
		} else if (strcmp(function, "set_property") == 0) {
			if (args.size() >= 3) {
				int64_t object_id = args[0].operator int64_t();
				String prop_name = args[1].operator String();
				Variant value = args[2];

				Object *obj = ObjectDB::get_instance(ObjectID((uint64_t)object_id));
				if (obj != nullptr) {
					obj->set(prop_name, value);
					ei_x_encode_atom(&reply, "ok");
				} else {
					ei_x_encode_tuple_header(&reply, 2);
					ei_x_encode_atom(&reply, "error");
					ei_x_encode_string(&reply, "object_not_found");
				}
			} else {
				ei_x_encode_tuple_header(&reply, 2);
				ei_x_encode_atom(&reply, "error");
				ei_x_encode_string(&reply, "insufficient_arguments");
			}
		} else {
			ei_x_encode_tuple_header(&reply, 2);
			ei_x_encode_atom(&reply, "error");
			ei_x_encode_string(&reply, "unknown_function");
		}
	} else if (strcmp(module, "erlang") == 0) {
		// Erlang built-in functions
		if (strcmp(function, "node") == 0) {
			// {call, erlang, node, []} - return the CNode's name
			// Reply format: just the node name atom (not wrapped in {reply, ...})
			ei_x_encode_atom(&reply, ec.thisnodename);
		} else if (strcmp(function, "nodes") == 0) {
			// {call, erlang, nodes, []} - return list of connected nodes
			// Reply format: just the list (not wrapped in {reply, ...})
			// Return empty list for now (CNode doesn't track connected nodes)
			ei_x_encode_list_header(&reply, 0);
			ei_x_encode_empty_list(&reply);
		} else {
			// Unknown function in erlang module
			ei_x_encode_tuple_header(&reply, 2);
			ei_x_encode_atom(&reply, "error");
			ei_x_encode_string(&reply, "unknown_function");
		}
	} else {
		// Unknown module
		ei_x_encode_tuple_header(&reply, 2);
		ei_x_encode_atom(&reply, "error");
		ei_x_encode_string(&reply, "unknown_module");
	}

	/* Send GenServer-style reply */
	send_reply(&reply, fd, from_pid, tag_ref);
	ei_x_free(&reply);

	return 0;
}

/*
 * Handle asynchronous cast from Erlang/Elixir (GenServer-like cast)
 */
static int handle_cast(char *buf, int *index) {
	// Guard: Check for null pointers
	if (buf == nullptr || index == nullptr) {
		fprintf(stderr, "Error: null pointer in handle_cast\n");
		return -1;
	}

	/* Decode Request: {Module, Function, Args} */
	int request_arity;
	if (ei_decode_tuple_header(buf, index, &request_arity) < 0 || request_arity < 2) {
		fprintf(stderr, "Error: invalid request format in gen_cast\n");
		return -1;
	}

	/* Decode Module and Function from Request tuple */
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

	// Decode arguments (remaining elements in Request tuple)
	Array args;
	if (request_arity > 2) {
		// Decode args array
		Variant args_variant = bert_to_variant(buf, index, true); // Skip version, already in tuple
		if (args_variant.get_type() == Variant::ARRAY) {
			args = args_variant.operator Array();
			printf("Godot CNode: handle_cast - Decoded args array with %lld elements\n", (long long)args.size());
		} else {
			// Single argument, wrap in array
			args.push_back(args_variant);
			printf("Godot CNode: handle_cast - Decoded single arg, wrapped in array\n");
		}
		fflush(stdout);
	}

	// Route based on module (async, no reply)
	printf("Godot CNode: Processing async message - Module: %s, Function: %s\n", module, function);
	fflush(stdout);

	if (strcmp(module, "erlang") == 0) {
		if (strcmp(function, "node") == 0) {
			printf("Godot CNode: Async erlang:node - Node name: %s\n", ec.thisnodename);
		} else if (strcmp(function, "nodes") == 0) {
			printf("Godot CNode: Async erlang:nodes - No other connected nodes\n");
		} else {
			printf("Godot CNode: Async erlang:%s - Unknown function\n", function);
		}
	} else if (strcmp(module, "godot") == 0) {
		// Generic Godot API calls - now safe since we're on main thread
		if (strcmp(function, "call_method") == 0) {
			if (args.size() >= 2) {
				int64_t object_id = args[0].operator int64_t();
				String method_name = args[1].operator String();
				Array method_args;
				if (args.size() > 2 && args[2].get_type() == Variant::ARRAY) {
					method_args = args[2].operator Array();
				}

				// Get object and call method (async, no return value) using callv() - no argument limit!
				Object *obj = ObjectDB::get_instance(ObjectID((uint64_t)object_id));
				if (obj != nullptr) {
					// Use callv() which accepts an Array of arguments - supports unlimited arguments
					obj->callv(method_name, method_args);
					printf("Godot CNode: Async godot:call_method - Success (called with %lld args)\n", (long long)method_args.size());
				} else {
					printf("Godot CNode: Async godot:call_method - Error: Object not found (ID: %lld)\n", (long long)object_id);
				}
			} else {
				printf("Godot CNode: Async godot:call_method - Error: Insufficient arguments\n");
			}
		} else if (strcmp(function, "set_property") == 0) {
			if (args.size() >= 3) {
				int64_t object_id = args[0].operator int64_t();
				String prop_name = args[1].operator String();
				Variant value = args[2];

				Object *obj = ObjectDB::get_instance(ObjectID((uint64_t)object_id));
				if (obj != nullptr) {
					obj->set(prop_name, value);
					printf("Godot CNode: Async godot:set_property - Success\n");
				} else {
					printf("Godot CNode: Async godot:set_property - Error: Object not found (ID: %lld)\n", (long long)object_id);
				}
			} else {
				printf("Godot CNode: Async godot:set_property - Error: Insufficient arguments\n");
			}
		} else {
			printf("Godot CNode: Async godot:%s - Unknown function\n", function);
		}
	} else {
		printf("Godot CNode: Async %s:%s - Unknown module\n", module, function);
	}

	printf("Godot CNode: Async message processing complete\n");
	fflush(stdout);
	return 0;
}

/*
 * Send reply to Erlang/Elixir (GenServer-style synchronous call)
 * Sends reply in format: {Tag, Reply} to the From PID
 */
static void send_reply(ei_x_buff *x, int fd, erlang_pid *to_pid, erlang_ref *tag_ref) {
	// Guard: Check for null pointer
	if (x == nullptr) {
		fprintf(stderr, "Error: null reply buffer in send_reply\n");
		return;
	}

	// Guard: Check for valid file descriptor
	if (fd < 0) {
		fprintf(stderr, "Error: invalid file descriptor in send_reply\n");
		return;
	}

	// Guard: Check for valid PID and tag
	if (to_pid == nullptr || tag_ref == nullptr) {
		fprintf(stderr, "Error: null PID or tag in send_reply\n");
		return;
	}

	/* Encode GenServer-style reply: {Tag, Reply} */
	/* The reply buffer (x) already contains the reply data, we need to wrap it with the tag */
	ei_x_buff gen_reply;
	/* Use ei_x_new_with_version to ensure BERT version byte (0x83) is included */
	/* This is required for ei_send to work correctly */
	ei_x_new_with_version(&gen_reply);

	/* Encode tuple header for {Tag, Reply} */
	ei_x_encode_tuple_header(&gen_reply, 2);

	/* Encode the tag (reference) */
	ei_x_encode_ref(&gen_reply, tag_ref);

	/* Append the reply data (which is already encoded in x) */
	/* Note: x->buff may or may not have version byte, but we're appending the encoded data */
	ei_x_append_buf(&gen_reply, x->buff, x->index);

	/* Debug: Print hex dump of reply buffer */
	printf("Godot CNode: Reply buffer (hex, first 64 bytes): ");
	for (int i = 0; i < gen_reply.index && i < 64; i++) {
		printf("%02x ", (unsigned char)gen_reply.buff[i]);
	}
	printf("\n");
	fflush(stdout);

	/* Send the GenServer-style reply to the From PID */
	/* Use ei_send to send to a specific PID on the connected socket */
	/* Format: ei_send(fd, pid, buf, len) */
	/* Note: ei_send handles the distribution protocol automatically, but the buffer should be a valid BERT term */
	/* The buffer should start with BERT version byte (0x83) which ei_x_new_with_version adds */
	int send_result = ei_send(fd, to_pid, gen_reply.buff, gen_reply.index);
	if (send_result < 0) {
		fprintf(stderr, "Error sending reply (errno: %d, %s)\n", errno, strerror(errno));
		fflush(stderr);
	} else {
		printf("Godot CNode: Reply sent successfully (GenServer format, %d bytes)\n", gen_reply.index);
		/* Flush the socket to ensure reply is sent immediately */
		fflush(stdout);
		/* Give the socket more time to send the data and for client to receive */
		usleep(100000); // 100ms - increased from 10ms
	}

	ei_x_free(&gen_reply);
}

/*
 * Main loop - listen for messages from Erlang/Elixir
 */
extern "C" {
void main_loop() {
	ei_x_buff x;
	erlang_msg msg;
	int fd;
	int res;

	printf("Godot CNode: Entering main loop\n");

	ei_x_new(&x);

	/* Check if listen_fd is valid before entering loop */
	if (listen_fd < 0) {
		fprintf(stderr, "Godot CNode: Invalid listen_fd: %d, cannot accept connections\n", listen_fd);
		return;
	}

	while (1) {
		/* Check if listen_fd is still valid (might be closed during shutdown) */
		if (listen_fd < 0) {
			printf("Godot CNode: listen_fd closed, exiting main loop\n");
			break;
		}

		/* Accept connection from Erlang/Elixir node */
		/* Use blocking ei_accept - this is the standard way for CNode servers */
		/* ei_accept() handles the Erlang distribution protocol handshake automatically */
		/* Custom socket callbacks provide macOS-compatible accept implementation */

		/* Use select() to wait for connection */
		fd_set read_fds;
		FD_ZERO(&read_fds);
		FD_SET(listen_fd, &read_fds);
		select(listen_fd + 1, &read_fds, NULL, NULL, NULL);

		if (!FD_ISSET(listen_fd, &read_fds)) {
			continue;
		}

		ErlConnect con;
		fd = ei_accept(&ec, listen_fd, &con);

		if (fd < 0) {
			int saved_errno = errno;
			fprintf(stderr, "Godot CNode: ei_accept() failed: %d (errno: %d, %s)\n", fd, saved_errno, strerror(saved_errno));

			/* Handle specific error codes */
			if (saved_errno == EBADF || saved_errno == 9) {
				/* Bad file descriptor - socket was closed */
				fprintf(stderr, "Godot CNode: listen_fd closed, exiting main loop\n");
				break;
			} else if (saved_errno == ECONNABORTED || saved_errno == 53) {
				/* Connection aborted - retry */
				printf("Godot CNode: Connection aborted (errno: %d), retrying...\n", saved_errno);
				continue;
			} else if (saved_errno == EINTR) {
				/* Interrupted by signal - retry */
				printf("Godot CNode: ei_accept() interrupted, retrying...\n");
				continue;
			} else {
				/* Other error - log details and retry after short delay */
				fprintf(stderr, "Godot CNode: ei_accept() error (errno: %d, %s), retrying after 100ms...\n", saved_errno, strerror(saved_errno));
				usleep(100000); // 100ms before retry
				continue;
			}
		}

		/* Log connection acceptance with timestamp for debugging */
		struct timeval accept_time;
		gettimeofday(&accept_time, NULL);
		printf("Godot CNode: ✓ Accepted connection on fd: %d at %ld.%06d\n", fd, accept_time.tv_sec, accept_time.tv_usec);
		fflush(stdout);
		if (con.nodename[0] != '\0') {
			printf("Godot CNode: Connected from node: %s\n", con.nodename);
		} else {
			printf("Godot CNode: Connected from node: (nodename not provided)\n");
		}
		fflush(stdout);

		/* Receive message - use select() to wait for data before calling ei_receive_msg */
		struct timeval wait_start;
		gettimeofday(&wait_start, NULL);
		printf("Godot CNode: Waiting to receive message from fd: %d at %ld.%06d...\n", fd, wait_start.tv_sec, wait_start.tv_usec);
		fflush(stdout);

		/* Wait for data to be available using select() */
		fd_set recv_fds;
		struct timeval recv_timeout;
		FD_ZERO(&recv_fds);
		FD_SET(fd, &recv_fds);
		recv_timeout.tv_sec = 5; /* 5 second timeout */
		recv_timeout.tv_usec = 0;
		int select_res = select(fd + 1, &recv_fds, NULL, NULL, &recv_timeout);
		bool data_available = (select_res > 0 && FD_ISSET(fd, &recv_fds));
		if (data_available) {
			/* Data is available, try to receive */
			printf("Godot CNode: select() indicates data available, calling ei_receive_msg...\n");
			fflush(stdout);
			res = ei_receive_msg(fd, &msg, &x);
			printf("Godot CNode: ei_receive_msg returned: %d", res);
			fflush(stdout);
		} else if (select_res == 0) {
			/* Timeout - no data available */
			printf("Godot CNode: select() timeout, no data available\n");
			fflush(stdout);
			ei_x_free(&x);
			close(fd);
			continue;
		} else {
			/* select() error */
			printf("Godot CNode: select() error (errno: %d, %s)\n", errno, strerror(errno));
			fflush(stdout);
			ei_x_free(&x);
			close(fd);
			continue;
		}

		if (res == ERL_TICK) {
			/* Just a tick, continue */
			printf(" (ERL_TICK - keepalive)\n");
			continue;
		} else if (res == ERL_ERROR) {
			int saved_errno = errno;
			fprintf(stderr, " (ERL_ERROR - errno: %d, %s)\n", saved_errno, strerror(saved_errno));
			/* macOS issue: errno 42 (Protocol not available) or errno 60 (Operation timed out) */
			/* When ei_receive_msg fails, try raw read - the error might just mean ei_receive_msg couldn't decode it */
			/* Note: errno 60 can occur even when data is available - ei_receive_msg just couldn't decode it */
			if (saved_errno == 42 || saved_errno == ENOPROTOOPT || saved_errno == 60 || saved_errno == ETIMEDOUT) {
				/* Try to process message from existing buffer if it has data */
				printf("Godot CNode: Buffer index: %d, attempting to process message despite errno %d (data_available=%d)\n", x.index, saved_errno, data_available);
				fflush(stdout);
				if (x.index > 0) {
					printf("Godot CNode: Attempting to process message from buffer (macOS compatibility, errno %d)\n", saved_errno);
					fflush(stdout);
					/* Process the message from buffer */
					x.index = 0;
					if (process_message(x.buff, &x.index, fd) < 0) {
						fprintf(stderr, "Error processing message\n");
					}
					ei_x_free(&x);
					close(fd);
					continue;
				} else {
					/* Buffer is empty but ei_receive_msg failed - try raw read anyway */
					/* Even if select() didn't indicate data, the error might mean there's data ei_receive_msg couldn't decode */
					printf("Godot CNode: Buffer is empty but ei_receive_msg failed, trying raw read (errno %d)...\n", saved_errno);
					fflush(stdout);
					unsigned char raw_buf[4096];
					ssize_t bytes_read = read(fd, raw_buf, sizeof(raw_buf));
					if (bytes_read > 0) {
						printf("Godot CNode: Raw read got %zd bytes, attempting to decode...\n", bytes_read);
						fflush(stdout);

						/* Convert raw data to base64 for inspection */
						const char base64_chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
						char base64_buf[8192]; /* Enough for 4096 bytes * 4/3 + padding */
						int base64_len = 0;
						for (ssize_t i = 0; i < bytes_read; i += 3) {
							unsigned char b1 = raw_buf[i];
							unsigned char b2 = (i + 1 < bytes_read) ? raw_buf[i + 1] : 0;
							unsigned char b3 = (i + 2 < bytes_read) ? raw_buf[i + 2] : 0;

							base64_buf[base64_len++] = base64_chars[(b1 >> 2) & 0x3F];
							base64_buf[base64_len++] = base64_chars[((b1 << 4) | (b2 >> 4)) & 0x3F];
							if (i + 1 < bytes_read) {
								base64_buf[base64_len++] = base64_chars[((b2 << 2) | (b3 >> 6)) & 0x3F];
							} else {
								base64_buf[base64_len++] = '=';
							}
							if (i + 2 < bytes_read) {
								base64_buf[base64_len++] = base64_chars[b3 & 0x3F];
							} else {
								base64_buf[base64_len++] = '=';
							}
						}
						base64_buf[base64_len] = '\0';
						printf("Godot CNode: Raw data (base64, FULL BUFFER): %s\n", base64_buf);
						fflush(stdout);

						/* Also print hex dump of first 64 bytes for inspection */
						printf("Godot CNode: Raw data (hex, first 64 bytes): ");
						for (ssize_t i = 0; i < bytes_read && i < 64; i++) {
							printf("%02x ", raw_buf[i]);
						}
						printf("\n");
						fflush(stdout);

						/* Print full hex dump for debugging */
						printf("Godot CNode: Raw data (hex, FULL BUFFER, %zd bytes):\n", bytes_read);
						for (ssize_t i = 0; i < bytes_read; i++) {
							if (i % 16 == 0) {
								printf("%04zx: ", i);
							}
							printf("%02x ", raw_buf[i]);
							if (i % 16 == 15 || i == bytes_read - 1) {
								printf("\n");
							}
						}
						fflush(stdout);

						/* The raw data is in Erlang distribution protocol format */
						/* Distribution protocol: [Length (4 bytes BE)] [Message Type] [Payload] */
						/* We need to skip the length and message type to get to the BERT payload */
						if (bytes_read >= 5) {
							/* Distribution protocol SEND format: [4-byte BE length] [1-byte 'p'] [To Name] [Message] */
							/* The "To Name" is encoded, then the actual BERT message follows */
							/* We need to skip the "To Name" to get to the actual message */
							int offset = 5; /* Skip header */

							/* Try to find ALL messages in the buffer */
							/* The distribution protocol format is: [4-byte length] [1-byte 'p'] [To Name] [Message] */
							/* The "To Name" is encoded as an atom (starts with 0x83), then the actual message follows */
							/* Look for pattern: 0x83 followed by 0x68 (small tuple) or 0x6B (large tuple) */
							/* Find all occurrences and process each message */
							printf("Godot CNode: Searching for all messages in buffer (size: %zd bytes, offset: %d)\n", bytes_read, offset);
							fflush(stdout);

							/* Find all 0x83 bytes that are followed by tuple markers */
							int msg_starts[10]; /* Max 10 messages */
							int msg_count = 0;
							for (int i = offset; i < bytes_read - 2 && msg_count < 10; i++) {
								if (raw_buf[i] == 0x83) {
									/* Check if this is followed by a tuple marker (0x68 = small tuple, 0x6B = large tuple) */
									if ((raw_buf[i + 1] == 0x68 || raw_buf[i + 1] == 0x6B)) {
										msg_starts[msg_count] = i;
										printf("Godot CNode: Found potential message at offset %d (hex: 0x%02x 0x%02x), message #%d\n",
												i, raw_buf[i], raw_buf[i + 1], msg_count + 1);
										fflush(stdout);
										msg_count++;
									}
								}
							}

							printf("Godot CNode: Found %d potential message(s) in buffer\n", msg_count);
							fflush(stdout);

							/* Process each message found */
							/* The first one (offset 5) is the "To Name" atom, skip it */
							/* The rest are actual messages */
							for (int msg_idx = 1; msg_idx < msg_count; msg_idx++) {
								int msg_start = msg_starts[msg_idx];
								printf("Godot CNode: Processing message #%d at offset %d\n", msg_idx, msg_start);
								fflush(stdout);

								/* Calculate payload length - from this message to end of buffer or next message */
								int payload_len;
								if (msg_idx + 1 < msg_count) {
									/* Next message starts at msg_starts[msg_idx + 1] */
									payload_len = msg_starts[msg_idx + 1] - msg_start;
								} else {
									/* Last message - goes to end of buffer */
									payload_len = (int)bytes_read - msg_start;
								}
								printf("Godot CNode: Found message at offset %d, extracted %d bytes, attempting to decode...\n", msg_start, payload_len);
								fflush(stdout);

								/* Print hex of message start for debugging */
								printf("Godot CNode: Message start (hex, first 32 bytes): ");
								for (int i = 0; i < 32 && (msg_start + i) < bytes_read; i++) {
									printf("%02x ", raw_buf[msg_start + i]);
								}
								printf("\n");
								fflush(stdout);

								/* The payload already has BERT version, so use it directly */
								ei_x_buff raw_x;
								ei_x_new(&raw_x); /* Don't add version, payload has it */
								ei_x_append_buf(&raw_x, (const char *)&raw_buf[msg_start], payload_len);
								raw_x.index = 0;

								/* The message from distribution protocol is already a tuple, so we need to decode it */
								/* Format: {'$gen_call', {From, Tag}, Request} or {rex, From, {'$gen_call', ...}} */
								/* Skip version (already at index 0, which is the 0x83 byte) */
								int msg_version;
								printf("Godot CNode: Attempting to decode version, index before: %d, first byte: 0x%02x\n", raw_x.index, (unsigned char)raw_x.buff[raw_x.index]);
								fflush(stdout);
								if (ei_decode_version(raw_x.buff, &raw_x.index, &msg_version) < 0) {
									fprintf(stderr, "Error: Could not decode BERT version from raw message (index: %d, first byte: 0x%02x)\n", raw_x.index, (unsigned char)raw_x.buff[0]);
									fflush(stderr);
									ei_x_free(&raw_x);
									continue;
								}
								printf("Godot CNode: Decoded version: %d, index after: %d, next byte: 0x%02x\n", msg_version, raw_x.index, (unsigned char)raw_x.buff[raw_x.index]);
								fflush(stdout);

								/* Decode tuple header */
								int tuple_arity;
								printf("Godot CNode: Attempting to decode tuple header, index: %d, bytes: 0x%02x 0x%02x\n", raw_x.index, (unsigned char)raw_x.buff[raw_x.index], (unsigned char)raw_x.buff[raw_x.index + 1]);
								fflush(stdout);
								if (ei_decode_tuple_header(raw_x.buff, &raw_x.index, &tuple_arity) < 0) {
									fprintf(stderr, "Error: Could not decode tuple header from raw message (index: %d)\n", raw_x.index);
									fflush(stderr);
									ei_x_free(&raw_x);
									continue;
								}
								printf("Godot CNode: Decoded tuple arity: %d, index after: %d\n", tuple_arity, raw_x.index);
								fflush(stdout);

								/* Decode first atom to determine message type */
								/* Check type first (this doesn't advance index) */
								int atom_type, atom_size;
								int atom_index = raw_x.index; /* Save position */
								if (ei_get_type(raw_x.buff, &atom_index, &atom_type, &atom_size) < 0) {
									fprintf(stderr, "Error: Could not get type at index %d\n", raw_x.index);
									fflush(stderr);
									ei_x_free(&raw_x);
									continue;
								}
								printf("Godot CNode: Type at index %d: 0x%02x (size: %d), index after get_type: %d\n", raw_x.index, atom_type, atom_size, atom_index);
								fflush(stdout);

								char first_atom[MAXATOMLEN];
								printf("Godot CNode: Attempting to decode atom, index: %d, bytes: 0x%02x 0x%02x 0x%02x\n", raw_x.index, (unsigned char)raw_x.buff[raw_x.index], (unsigned char)raw_x.buff[raw_x.index + 1], (unsigned char)raw_x.buff[raw_x.index + 2]);
								fflush(stdout);

								/* Decode atom - 0x6b is ATOM_UTF8_EXT (2-byte length) */
								/* ei_decode_atom might not support UTF-8, try manual parse */
								int decode_res;
								if (atom_type == 0x6b) {
									/* ATOM_UTF8_EXT: [0x6b] [len_high] [len_low] [utf8_bytes...] */
									unsigned char len_high = raw_x.buff[raw_x.index + 1];
									unsigned char len_low = raw_x.buff[raw_x.index + 2];
									int atom_len = (len_high << 8) | len_low;
									if (atom_len > 0 && atom_len < MAXATOMLEN) {
										memcpy(first_atom, &raw_x.buff[raw_x.index + 3], atom_len);
										first_atom[atom_len] = '\0';
										raw_x.index += 3 + atom_len; /* Skip type + 2-byte len + data */
										decode_res = 0;
										printf("Godot CNode: Manually decoded UTF-8 atom: '%s' (len: %d)\n", first_atom, atom_len);
										fflush(stdout);
									} else {
										decode_res = -1;
									}
								} else {
									/* Try standard ei_decode_atom */
									decode_res = ei_decode_atom(raw_x.buff, &raw_x.index, first_atom);
								}

								if (decode_res < 0) {
									fprintf(stderr, "Error: Could not decode first atom from raw message (index: %d, type: 0x%02x, bytes: 0x%02x 0x%02x 0x%02x)\n", raw_x.index, atom_type, (unsigned char)raw_x.buff[raw_x.index], (unsigned char)raw_x.buff[raw_x.index + 1], (unsigned char)raw_x.buff[raw_x.index + 2]);
									fflush(stderr);
									ei_x_free(&raw_x);
									continue;
								}

								printf("Godot CNode: Raw message - tuple arity: %d, first atom: %s\n", tuple_arity, first_atom);
								fflush(stdout);

								/* Now process based on message type */
								if (strcmp(first_atom, "$gen_call") == 0) {
									/* Direct gen_call: {'$gen_call', {From, Tag}, Request} */
									printf("Godot CNode: Processing direct $gen_call from raw message\n");
									fflush(stdout);
									/* Decode {From, Tag} */
									int from_arity;
									if (ei_decode_tuple_header(raw_x.buff, &raw_x.index, &from_arity) < 0 || from_arity != 2) {
										fprintf(stderr, "Error decoding From tuple in raw gen_call\n");
										fflush(stderr);
										ei_x_free(&raw_x);
										continue;
									}
									erlang_pid from_pid;
									erlang_ref tag_ref;
									if (ei_decode_pid(raw_x.buff, &raw_x.index, &from_pid) < 0) {
										fprintf(stderr, "Error decoding From PID in raw gen_call\n");
										fflush(stderr);
										ei_x_free(&raw_x);
										continue;
									}
									if (ei_decode_ref(raw_x.buff, &raw_x.index, &tag_ref) < 0) {
										fprintf(stderr, "Error decoding Tag in raw gen_call\n");
										fflush(stderr);
										ei_x_free(&raw_x);
										continue;
									}
									/* Now handle the Request */
									printf("Godot CNode: About to call handle_call for raw message (From PID: %s, Tag: %p)\n",
											from_pid.node, (void *)&tag_ref);
									fflush(stdout);
									int call_result = handle_call(raw_x.buff, &raw_x.index, fd, &from_pid, &tag_ref);
									if (call_result < 0) {
										fprintf(stderr, "Error handling call from raw message\n");
										fflush(stderr);
									} else {
										printf("Godot CNode: handle_call succeeded for raw message, reply should have been sent\n");
										fflush(stdout);
										/* GenServer call succeeded - give more time for reply to be sent and received */
										usleep(200000); // 200ms - increased from 50ms
									}
								} else if (strcmp(first_atom, "rex") == 0) {
									/* RPC wrapped: {rex, From, {'$gen_call', ...}} */
									printf("Godot CNode: Processing rex message from raw message\n");
									fflush(stdout);
									/* Use process_message which handles rex format */
									raw_x.index = 0; /* Reset to start */
									int rex_result = process_message(raw_x.buff, &raw_x.index, fd);
									if (rex_result < 0) {
										fprintf(stderr, "Error processing rex message from raw read payload\n");
										fflush(stderr);
									} else {
										/* GenServer call succeeded - give more time for reply to be sent and received */
										usleep(200000); // 200ms - increased from 50ms
									}
								} else {
									fprintf(stderr, "Error: Unknown message type in raw message: %s\n", first_atom);
									fflush(stderr);
								}

								ei_x_free(&raw_x);
								printf("Godot CNode: Finished processing message #%d\n", msg_idx);
								fflush(stdout);
							}

							if (msg_count == 0) {
								printf("Godot CNode: Could not find any messages (BERT version 0x83) in payload\n");
								fflush(stdout);
							} else if (msg_count == 1) {
								printf("Godot CNode: Found only 'To Name', no actual messages in buffer\n");
								fflush(stdout);
							} else {
								printf("Godot CNode: Processed %d message(s) from buffer (skipped 'To Name')\n", msg_count - 1);
								fflush(stdout);
							}
						} else {
							printf("Godot CNode: Raw data too short (%zd bytes), expected at least 5\n", bytes_read);
							fflush(stdout);
						}
					} else {
						printf("Godot CNode: Raw read failed (bytes_read: %zd, errno: %d)\n", bytes_read, errno);
						fflush(stdout);
					}
				}
			}
			ei_x_free(&x);
			/* Give additional time for reply to be fully transmitted before closing */
			usleep(200000); // 200ms - increased from 100ms
			close(fd);
			continue;
		} else {
			struct timeval receive_time;
			gettimeofday(&receive_time, NULL);
			long elapsed_us = (receive_time.tv_sec - wait_start.tv_sec) * 1000000 + (receive_time.tv_usec - wait_start.tv_usec);
			printf(" (success, elapsed: %ld us)\n", elapsed_us);
		}

		printf("Godot CNode: Message type: %ld\n", msg.msgtype);

		/* Process the message */
		x.index = 0;
		int process_result = process_message(x.buff, &x.index, fd);
		if (process_result < 0) {
			fprintf(stderr, "Error processing message\n");
		}

		/* For GenServer calls, give time for reply to be sent */
		if (process_result == 0) {
			/* Longer delay to ensure reply is fully sent and received for GenServer calls */
			usleep(200000); // 200ms - increased from 50ms
		}

		/* Process multiple messages on the same connection */
		/* According to Erlang distribution protocol, multiple messages can be sent on the same connection */
		/* Continue receiving messages until select() times out or connection closes */
		printf("Godot CNode: Checking for more messages on this connection...\n");
		fflush(stdout);

		/* Free the current buffer and prepare for next message */
		ei_x_free(&x);
		ei_x_new(&x);

		/* Check if more data is available with a short timeout */
		FD_ZERO(&recv_fds);
		FD_SET(fd, &recv_fds);
		recv_timeout.tv_sec = 0; /* No wait - check immediately */
		recv_timeout.tv_usec = 100000; /* 100ms */
		select_res = select(fd + 1, &recv_fds, NULL, NULL, &recv_timeout);
		if (select_res > 0 && FD_ISSET(fd, &recv_fds)) {
			/* More data available - continue processing */
			printf("Godot CNode: More data available, continuing to receive...\n");
			fflush(stdout);
			continue; /* Go back to receive next message */
		} else {
			/* No more data - close connection */
			printf("Godot CNode: No more data available, closing connection\n");
			fflush(stdout);
			ei_x_free(&x);
			close(fd);
		}
	}

	ei_x_free(&x);
}
} // extern "C" - closes main_loop's extern "C" block

/*
 * Non-blocking version of main_loop for use in Godot's main thread
 * Processes one connection or message per call, returns immediately if nothing available
 * Returns: 0 = processed something, 1 = nothing to process, -1 = error/shutdown
 */
extern "C" {
int process_cnode_frame(void) {
	static ei_x_buff x;
	static erlang_msg msg;
	static int current_fd = -1;
	static bool x_initialized = false;

	// Initialize buffer on first call
	if (!x_initialized) {
		ei_x_new(&x);
		x_initialized = true;
	}

	// Check if listen_fd is valid
	if (listen_fd < 0) {
		return -1; // Shutdown
	}

	// If we have an active connection, try to process messages on it first
	if (current_fd >= 0) {
		// Check if more data is available with zero timeout (non-blocking)
		fd_set recv_fds;
		struct timeval recv_timeout;
		FD_ZERO(&recv_fds);
		FD_SET(current_fd, &recv_fds);
		recv_timeout.tv_sec = 0;
		recv_timeout.tv_usec = 0; // Zero timeout = non-blocking
		int select_res = select(current_fd + 1, &recv_fds, NULL, NULL, &recv_timeout);

		if (select_res > 0 && FD_ISSET(current_fd, &recv_fds)) {
			// Data available - try to receive message
			int res = ei_receive_msg(current_fd, &msg, &x);

			if (res == ERL_TICK) {
				// Just a tick, continue
				return 1; // Nothing to process
			} else if (res == ERL_ERROR) {
				// Error or connection closed
				int saved_errno = errno;
				if (saved_errno == 42 || saved_errno == ENOPROTOOPT) {
					// macOS compatibility - try to process from buffer
					if (x.index > 0) {
						x.index = 0;
						if (process_message(x.buff, &x.index, current_fd) >= 0) {
							ei_x_free(&x);
							ei_x_new(&x);
							return 0; // Processed message
						}
					}
				}
				// Connection closed or error - close and reset
				close(current_fd);
				current_fd = -1;
				ei_x_free(&x);
				ei_x_new(&x);
				return 1; // Nothing more to process
			} else if (res == ERL_MSG) {
				// Message received - process it
				x.index = 0;
				int process_result = process_message(x.buff, &x.index, current_fd);

				// Free buffer and prepare for next message
				ei_x_free(&x);
				ei_x_new(&x);

				if (process_result < 0) {
					// Error processing - close connection
					close(current_fd);
					current_fd = -1;
					return -1;
				}

				return 0; // Processed message
			}
		} else if (select_res == 0) {
			// No data available - check if connection is still alive
			// For now, keep connection open for next frame
			return 1; // Nothing to process this frame
		} else {
			// select() error - close connection
			close(current_fd);
			current_fd = -1;
			ei_x_free(&x);
			ei_x_new(&x);
			return 1;
		}
	}

	// No active connection - check for new connections (non-blocking)
	fd_set read_fds;
	struct timeval timeout;
	FD_ZERO(&read_fds);
	FD_SET(listen_fd, &read_fds);
	timeout.tv_sec = 0;
	timeout.tv_usec = 0; // Zero timeout = non-blocking

	int select_res = select(listen_fd + 1, &read_fds, NULL, NULL, &timeout);

	if (select_res > 0 && FD_ISSET(listen_fd, &read_fds)) {
		// New connection available - accept it
		ErlConnect con;
		int fd = ei_accept(&ec, listen_fd, &con);

		if (fd >= 0) {
			printf("Godot CNode: ✓ Accepted connection on fd: %d\n", fd);
			fflush(stdout);
			if (con.nodename[0] != '\0') {
				printf("Godot CNode: Connected from node: %s\n", con.nodename);
				fflush(stdout);
			}

			// Store connection for next frame
			current_fd = fd;

			// Check if data is immediately available
			FD_ZERO(&read_fds);
			FD_SET(fd, &read_fds);
			timeout.tv_sec = 0;
			timeout.tv_usec = 0;
			select_res = select(fd + 1, &read_fds, NULL, NULL, &timeout);

			if (select_res > 0 && FD_ISSET(fd, &read_fds)) {
				// Data immediately available - process it
				int res = ei_receive_msg(fd, &msg, &x);

				if (res == ERL_MSG) {
					x.index = 0;
					int process_result = process_message(x.buff, &x.index, fd);
					ei_x_free(&x);
					ei_x_new(&x);

					if (process_result < 0) {
						close(fd);
						current_fd = -1;
						return -1;
					}

					return 0; // Processed message
				} else if (res == ERL_TICK) {
					return 1; // Just a tick
				} else {
					// Error - close connection
					close(fd);
					current_fd = -1;
					return 1;
				}
			}

			return 0; // Accepted connection
		} else {
			// Accept failed - but not critical, just try again next frame
			int saved_errno = errno;
			if (saved_errno == EBADF || saved_errno == 9) {
				// Socket closed - shutdown
				return -1;
			}
			// Other errors - just try again next frame
			return 1;
		}
	}

	// Nothing to process this frame
	return 1;
}
} // extern "C"

// CNodeServer Node class implementation
namespace godot {

void CNodeServer::_bind_methods() {
	ClassDB::bind_method(D_METHOD("_add_to_scene_tree"), &CNodeServer::_add_to_scene_tree);
}

CNodeServer::CNodeServer() : initialized(false), cookie_copy(nullptr) {
}

CNodeServer::~CNodeServer() {
	if (cookie_copy != nullptr) {
		delete[] cookie_copy;
		cookie_copy = nullptr;
	}
}

void CNodeServer::_add_to_scene_tree() {
	// Add this node to the scene tree
	Engine *engine = Engine::get_singleton();
	if (engine == nullptr) {
		UtilityFunctions::printerr("Godot CNode: Engine not available in _add_to_scene_tree");
		return;
	}

	MainLoop *main_loop = engine->get_main_loop();
	if (main_loop == nullptr) {
		UtilityFunctions::printerr("Godot CNode: Main loop not available in _add_to_scene_tree");
		return;
	}

	SceneTree *scene_tree = Object::cast_to<SceneTree>(main_loop);
	if (scene_tree == nullptr) {
		UtilityFunctions::printerr("Godot CNode: SceneTree not available in _add_to_scene_tree");
		return;
	}

	// Add to root window
	Window *root = scene_tree->get_root();
	if (root != nullptr && get_parent() == nullptr) {
		root->add_child(this);
		UtilityFunctions::print("Godot CNode: CNodeServer node added to root window");
	} else if (get_parent() == nullptr) {
		// Try current scene as fallback
		Node *current_scene = scene_tree->get_current_scene();
		if (current_scene != nullptr) {
			current_scene->add_child(this);
			UtilityFunctions::print("Godot CNode: CNodeServer node added to current scene");
		} else {
			UtilityFunctions::printerr("Godot CNode: Could not add CNodeServer node to scene tree");
		}
	}
}

void CNodeServer::_ready() {
	// Initialize CNode on main thread
	UtilityFunctions::print("Godot CNode: CNodeServer node ready, initializing CNode...");

	// Get cookie from environment or use default
	String cookie;
	OS *os = OS::get_singleton();
	if (os) {
		String env_cookie = os->get_environment("GODOT_CNODE_COOKIE");
		if (!env_cookie.is_empty()) {
			cookie = env_cookie.strip_edges();
			UtilityFunctions::print("Godot CNode: Using cookie from GODOT_CNODE_COOKIE environment variable");
		}
	}

	if (cookie.is_empty()) {
		// Use default cookie
		cookie = "godotcookie";
		UtilityFunctions::print("Godot CNode: Using default cookie");
	}

	// Store cookie as C string
	size_t cookie_len = cookie.length();
	cookie_copy = new char[cookie_len + 1];
	memcpy(cookie_copy, cookie.utf8().get_data(), cookie_len);
	cookie_copy[cookie_len] = '\0';

	// Initialize instances array
	memset(instances, 0, sizeof(instances));

	// Try different hostname options
	char hostname[256] = { 0 };
	char nodename[512] = { 0 };
	const char *nodename_options[4];
	int option_count = 0;

	nodename_options[option_count++] = "godot@127.0.0.1";
	nodename_options[option_count++] = "godot@localhost";

#ifndef _WIN32
	if (gethostname(hostname, sizeof(hostname) - 1) == 0) {
		if (strlen(hostname) > 6 && strcmp(hostname + strlen(hostname) - 6, ".local") != 0) {
			snprintf(nodename, sizeof(nodename), "godot@%s", hostname);
			nodename_options[option_count++] = nodename;
		}
	}
#endif
	nodename_options[option_count] = nullptr;

	bool init_success = false;
	for (int i = 0; nodename_options[i] != nullptr; i++) {
		if (init_cnode(const_cast<char *>(nodename_options[i]), cookie_copy) == 0) {
			init_success = true;
			UtilityFunctions::print(String("Godot CNode: Successfully initialized with ") + nodename_options[i]);
			break;
		}
	}

	if (!init_success) {
		UtilityFunctions::printerr("Godot CNode: Failed to initialize CNode with all hostname options");
		return;
	}

	initialized = true;
	UtilityFunctions::print(String("Godot CNode: CNodeServer initialized and ready (listen_fd: ") + itos(listen_fd) + ")");
}

void CNodeServer::_process(double delta) {
	if (!initialized || listen_fd < 0) {
		return;
	}

	// Process one frame of CNode operations (non-blocking)
	int result = process_cnode_frame();

	// result: 0 = processed something, 1 = nothing to process, -1 = error/shutdown
	if (result < 0) {
		// Error or shutdown
		UtilityFunctions::printerr("Godot CNode: process_cnode_frame() returned error, shutting down");
		initialized = false;
	}
}

} // namespace godot
