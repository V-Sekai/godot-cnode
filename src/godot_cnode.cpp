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
#include <CommonCrypto/CommonDigest.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#define MD5_DIGEST_LENGTH CC_MD5_DIGEST_LENGTH
#define MD5_CTX CC_MD5_CTX
#define MD5_Init CC_MD5_Init
#define MD5_Update CC_MD5_Update
#define MD5_Final CC_MD5_Final
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
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

// Godot-cpp includes
#include <godot_cpp/classes/class_db_singleton.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/main_loop.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
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
static Variant bert_to_variant(char *buf, int *index) {
	int type, arity;
	char atom[MAXATOMLEN];
	long long_val;
	double double_val;
	char string_buf[256];

	if (ei_decode_version(buf, index, NULL) < 0) {
		return Variant(); // Error
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
			if (arity == 0) {
				ei_decode_list_header(buf, index, &arity);
				return Variant(Array());
			} else {
				Array arr;
				ei_decode_list_header(buf, index, &arity);
				for (int i = 0; i < arity; i++) {
					Variant elem = bert_to_variant(buf, index);
					arr.push_back(elem);
				}
				// Check and skip the list tail (should be nil/empty list)
				int type, size;
				if (ei_get_type(buf, index, &type, &size) == 0 && type == ERL_NIL_EXT) {
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
							Variant key = bert_to_variant(buf, index);
							Variant value = bert_to_variant(buf, index);
							dict[key] = value;
						}
						return Variant(dict);
					}
				}
			}
			break;

		case ERL_NIL_EXT:
			return Variant();
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
static int handle_call(char *buf, int *index, int fd);
static int handle_cast(char *buf, int *index);
static void send_reply(ei_x_buff *x, int fd);

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

	/* ei_connect_init returns 0 on success, negative on error */
	res = ei_connect_init(&ec, nodename, cookie, 0);
	if (res < 0) {
		fprintf(stderr, "ei_connect_init failed: %d (errno: %d, %s)\n", res, errno, strerror(errno));
		fprintf(stderr, "  nodename: %s\n", nodename);
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

	printf("Godot CNode: Socket ready for accepting connections (fd: %d, port: %d)\n", listen_fd, port);
	return 0;
}
} // extern "C"

/*
 * Process incoming message from Erlang/Elixir
 * Handles both GenServer-style messages {call, ...} / {cast, ...}
 * and direct RPC calls from :rpc.call
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

	int arity;
	char atom[MAXATOMLEN];
	int saved_index = *index;

	/* Decode the message */
	if (ei_decode_version(buf, index, NULL) < 0) {
		fprintf(stderr, "Error decoding version\n");
		return -1;
	}

	if (ei_decode_tuple_header(buf, index, &arity) < 0) {
		fprintf(stderr, "Error decoding tuple header\n");
		return -1;
	}

	/* Get the message type atom */
	if (ei_decode_atom(buf, index, atom) < 0) {
		fprintf(stderr, "Error decoding atom\n");
		return -1;
	}

	/* Handle GenServer-style messages: {'$gen_call', {From, Tag}, Request} or {'$gen_cast', Request} */
	if (strcmp(atom, "$gen_call") == 0) {
		/* GenServer call: {'$gen_call', {From, Tag}, Request} */
		// Decode the From tuple {From, Tag}
		int from_arity;
		if (ei_decode_tuple_header(buf, index, &from_arity) < 0 || from_arity != 2) {
			fprintf(stderr, "Error decoding From tuple in gen_call\n");
			return -1;
		}
		// Skip From (PID) and Tag (reference) - we don't need them for CNode
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
		// Now decode the Request
		return handle_call(buf, index, fd);
	} else if (strcmp(atom, "$gen_cast") == 0) {
		/* GenServer cast: {'$gen_cast', Request} */
		// Request is directly after the atom
		return handle_cast(buf, index);
	} else {
		/* Only GenServer-style messages are supported */
		fprintf(stderr, "Only GenServer-style messages supported. Got: %s\n", atom);
		return -1;
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
 * Handle synchronous call from Erlang/Elixir
 */
static int handle_call(char *buf, int *index, int fd) {
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
		ei_x_encode_tuple_header(&reply, 2);
		ei_x_encode_atom(&reply, "error");
		ei_x_encode_string(&reply, "invalid_request_format");
		send_reply(&reply, fd);
		ei_x_free(&reply);
		return -1;
	}

	/* Decode Module and Function from Request tuple */
	char module[256];
	char function[256];

	if (ei_decode_atom(buf, index, module) < 0) {
		ei_x_encode_tuple_header(&reply, 2);
		ei_x_encode_atom(&reply, "error");
		ei_x_encode_string(&reply, "invalid_module");
		send_reply(&reply, fd);
		ei_x_free(&reply);
		return -1;
	}

	if (ei_decode_atom(buf, index, function) < 0) {
		ei_x_encode_tuple_header(&reply, 2);
		ei_x_encode_atom(&reply, "error");
		ei_x_encode_string(&reply, "invalid_function");
		send_reply(&reply, fd);
		ei_x_free(&reply);
		return -1;
	}

	// Decode arguments (remaining elements in Request tuple)
	Array args;
	if (request_arity > 2) {
		Variant args_array = bert_to_variant(buf, index);
		if (args_array.get_type() == Variant::ARRAY) {
			args = args_array.operator Array();
		}
	}

	// Route based on module
	if (strcmp(module, "godot") == 0) {
		// Generic Godot API calls
		if (strcmp(function, "call_method") == 0) {
			// {call, godot, call_method, [ObjectID, MethodName, Args]}
			if (args.size() < 2) {
				ei_x_encode_tuple_header(&reply, 2);
				ei_x_encode_atom(&reply, "error");
				ei_x_encode_string(&reply, "insufficient_args");
			} else {
				int64_t object_id = args[0].operator int64_t();
				String method_name = args[1].operator String();

				// Guard: Check for valid object ID
				if (object_id == 0) {
					ei_x_encode_tuple_header(&reply, 2);
					ei_x_encode_atom(&reply, "error");
					ei_x_encode_string(&reply, "invalid_object_id");
				} else if (method_name.is_empty()) {
					// Guard: Check method name is not empty
					ei_x_encode_tuple_header(&reply, 2);
					ei_x_encode_atom(&reply, "error");
					ei_x_encode_string(&reply, "empty_method_name");
				} else {
					Array method_args;
					if (args.size() > 2 && args[2].get_type() == Variant::ARRAY) {
						method_args = args[2].operator Array();
					}

					Object *obj = get_object_by_id(object_id);
					if (obj == nullptr) {
						ei_x_encode_tuple_header(&reply, 2);
						ei_x_encode_atom(&reply, "error");
						ei_x_encode_string(&reply, "object_not_found");
					} else {
						Variant result = obj->callv(method_name, method_args);
						ei_x_encode_tuple_header(&reply, 2);
						ei_x_encode_atom(&reply, "reply");
						variant_to_bert(result, &reply);
					}
				}
			}
		} else if (strcmp(function, "get_property") == 0) {
			// {call, godot, get_property, [ObjectID, PropertyName]}
			if (args.size() < 2) {
				ei_x_encode_tuple_header(&reply, 2);
				ei_x_encode_atom(&reply, "error");
				ei_x_encode_string(&reply, "insufficient_args");
			} else {
				int64_t object_id = args[0].operator int64_t();
				String prop_name = args[1].operator String();

				// Guard: Check for valid object ID
				if (object_id == 0) {
					ei_x_encode_tuple_header(&reply, 2);
					ei_x_encode_atom(&reply, "error");
					ei_x_encode_string(&reply, "invalid_object_id");
				} else if (prop_name.is_empty()) {
					// Guard: Check property name is not empty
					ei_x_encode_tuple_header(&reply, 2);
					ei_x_encode_atom(&reply, "error");
					ei_x_encode_string(&reply, "empty_property_name");
				} else {
					Object *obj = get_object_by_id(object_id);
					if (obj == nullptr) {
						ei_x_encode_tuple_header(&reply, 2);
						ei_x_encode_atom(&reply, "error");
						ei_x_encode_string(&reply, "object_not_found");
					} else {
						Variant value = obj->get(prop_name);
						ei_x_encode_tuple_header(&reply, 2);
						ei_x_encode_atom(&reply, "reply");
						variant_to_bert(value, &reply);
					}
				}
			}
		} else if (strcmp(function, "set_property") == 0) {
			// {call, godot, set_property, [ObjectID, PropertyName, Value]}
			if (args.size() < 3) {
				ei_x_encode_tuple_header(&reply, 2);
				ei_x_encode_atom(&reply, "error");
				ei_x_encode_string(&reply, "insufficient_args");
			} else {
				int64_t object_id = args[0].operator int64_t();
				String prop_name = args[1].operator String();
				Variant value = args[2];

				// Guard: Check for valid object ID
				if (object_id == 0) {
					ei_x_encode_tuple_header(&reply, 2);
					ei_x_encode_atom(&reply, "error");
					ei_x_encode_string(&reply, "invalid_object_id");
				} else if (prop_name.is_empty()) {
					// Guard: Check property name is not empty
					ei_x_encode_tuple_header(&reply, 2);
					ei_x_encode_atom(&reply, "error");
					ei_x_encode_string(&reply, "empty_property_name");
				} else {
					Object *obj = get_object_by_id(object_id);
					if (obj == nullptr) {
						ei_x_encode_tuple_header(&reply, 2);
						ei_x_encode_atom(&reply, "error");
						ei_x_encode_string(&reply, "object_not_found");
					} else {
						obj->set(prop_name, value);
						ei_x_encode_tuple_header(&reply, 2);
						ei_x_encode_atom(&reply, "reply");
						ei_x_encode_atom(&reply, "ok");
					}
				}
			}
		} else if (strcmp(function, "get_singleton") == 0) {
			// {call, godot, get_singleton, [SingletonName]}
			if (args.size() < 1) {
				ei_x_encode_tuple_header(&reply, 2);
				ei_x_encode_atom(&reply, "error");
				ei_x_encode_string(&reply, "insufficient_args");
			} else {
				String singleton_name = args[0].operator String();
				StringName name(singleton_name);
				GDExtensionObjectPtr singleton_obj = internal::gdextension_interface_global_get_singleton(name._native_ptr());
				if (singleton_obj == nullptr) {
					ei_x_encode_tuple_header(&reply, 2);
					ei_x_encode_atom(&reply, "error");
					ei_x_encode_string(&reply, "singleton_not_found");
				} else {
					Object *singleton = internal::get_object_instance_binding(singleton_obj);
					if (singleton == nullptr) {
						ei_x_encode_tuple_header(&reply, 2);
						ei_x_encode_atom(&reply, "error");
						ei_x_encode_string(&reply, "singleton_binding_failed");
					} else {
						ei_x_encode_tuple_header(&reply, 2);
						ei_x_encode_atom(&reply, "reply");
						ei_x_encode_tuple_header(&reply, 3);
						ei_x_encode_atom(&reply, "object");
						ei_x_encode_string(&reply, singleton->get_class().utf8().get_data());
						ei_x_encode_long(&reply, (long)(int64_t)singleton->get_instance_id());
					}
				}
			}
		} else if (strcmp(function, "create_object") == 0) {
			// {call, godot, create_object, [ClassName]}
			if (args.size() < 1) {
				ei_x_encode_tuple_header(&reply, 2);
				ei_x_encode_atom(&reply, "error");
				ei_x_encode_string(&reply, "insufficient_args");
			} else {
				String class_name = args[0].operator String();
				ClassDBSingleton *class_db = ClassDBSingleton::get_singleton();
				if (class_db == nullptr) {
					ei_x_encode_tuple_header(&reply, 2);
					ei_x_encode_atom(&reply, "error");
					ei_x_encode_string(&reply, "classdb_unavailable");
				} else {
					// Use ClassDB to instantiate the class
					Object *obj = ClassDB::instantiate(StringName(class_name));
					if (obj == nullptr) {
						ei_x_encode_tuple_header(&reply, 2);
						ei_x_encode_atom(&reply, "error");
						ei_x_encode_string(&reply, "class_not_found_or_not_instantiable");
					} else {
						ei_x_encode_tuple_header(&reply, 2);
						ei_x_encode_atom(&reply, "reply");
						ei_x_encode_tuple_header(&reply, 3);
						ei_x_encode_atom(&reply, "object");
						ei_x_encode_string(&reply, obj->get_class().utf8().get_data());
						ei_x_encode_long(&reply, (int64_t)obj->get_instance_id());
					}
				}
			}
		} else if (strcmp(function, "list_classes") == 0) {
			// {call, godot, list_classes, []}
			ClassDBSingleton *class_db = ClassDBSingleton::get_singleton();
			if (class_db == nullptr) {
				ei_x_encode_tuple_header(&reply, 2);
				ei_x_encode_atom(&reply, "error");
				ei_x_encode_string(&reply, "classdb_unavailable");
			} else {
				Array classes = class_db->get_class_list();
				ei_x_encode_tuple_header(&reply, 2);
				ei_x_encode_atom(&reply, "reply");
				ei_x_encode_list_header(&reply, classes.size());
				for (int i = 0; i < classes.size(); i++) {
					String class_name = classes[i];
					ei_x_encode_string(&reply, class_name.utf8().get_data());
				}
				ei_x_encode_empty_list(&reply);
			}
		} else if (strcmp(function, "get_class_methods") == 0) {
			// {call, godot, get_class_methods, [ClassName]}
			if (args.size() < 1) {
				ei_x_encode_tuple_header(&reply, 2);
				ei_x_encode_atom(&reply, "error");
				ei_x_encode_string(&reply, "insufficient_args");
			} else {
				String class_name = args[0].operator String();
				ClassDBSingleton *class_db = ClassDBSingleton::get_singleton();
				if (class_db == nullptr) {
					ei_x_encode_tuple_header(&reply, 2);
					ei_x_encode_atom(&reply, "error");
					ei_x_encode_string(&reply, "classdb_unavailable");
				} else {
					TypedArray<Dictionary> methods = class_db->class_get_method_list(class_name, true);
					ei_x_encode_tuple_header(&reply, 2);
					ei_x_encode_atom(&reply, "reply");
					ei_x_encode_list_header(&reply, methods.size());
					for (int i = 0; i < methods.size(); i++) {
						encode_method_info(methods[i], &reply);
					}
					ei_x_encode_empty_list(&reply);
				}
			}
		} else if (strcmp(function, "get_class_properties") == 0) {
			// {call, godot, get_class_properties, [ClassName]}
			if (args.size() < 1) {
				ei_x_encode_tuple_header(&reply, 2);
				ei_x_encode_atom(&reply, "error");
				ei_x_encode_string(&reply, "insufficient_args");
			} else {
				String class_name = args[0].operator String();
				ClassDBSingleton *class_db = ClassDBSingleton::get_singleton();
				if (class_db == nullptr) {
					ei_x_encode_tuple_header(&reply, 2);
					ei_x_encode_atom(&reply, "error");
					ei_x_encode_string(&reply, "classdb_unavailable");
				} else {
					TypedArray<Dictionary> properties = class_db->class_get_property_list(class_name, true);
					ei_x_encode_tuple_header(&reply, 2);
					ei_x_encode_atom(&reply, "reply");
					ei_x_encode_list_header(&reply, properties.size());
					for (int i = 0; i < properties.size(); i++) {
						Dictionary prop = properties[i];
						// Skip NIL properties (grouping/category markers)
						if (prop["type"].operator int() != Variant::NIL) {
							encode_property_info(prop, &reply);
						}
					}
					ei_x_encode_empty_list(&reply);
				}
			}
		} else if (strcmp(function, "get_singletons") == 0) {
			// {call, godot, get_singletons, []}
			Engine *engine = Engine::get_singleton();
			if (engine == nullptr) {
				ei_x_encode_tuple_header(&reply, 2);
				ei_x_encode_atom(&reply, "error");
				ei_x_encode_string(&reply, "engine_unavailable");
			} else {
				PackedStringArray singletons = engine->get_singleton_list();
				ei_x_encode_tuple_header(&reply, 2);
				ei_x_encode_atom(&reply, "reply");
				ei_x_encode_list_header(&reply, singletons.size());
				for (int i = 0; i < singletons.size(); i++) {
					ei_x_encode_string(&reply, singletons[i].utf8().get_data());
				}
				ei_x_encode_empty_list(&reply);
			}
		} else if (strcmp(function, "get_scene_tree_root") == 0) {
			// {call, godot, get_scene_tree_root, []}
			inst = get_current_instance();
			if (inst == nullptr || inst->scene_tree == nullptr) {
				ei_x_encode_tuple_header(&reply, 2);
				ei_x_encode_atom(&reply, "error");
				ei_x_encode_string(&reply, "no_scene_tree");
			} else {
				Node *root = get_scene_tree_root(inst->scene_tree);
				if (root == nullptr) {
					ei_x_encode_tuple_header(&reply, 2);
					ei_x_encode_atom(&reply, "error");
					ei_x_encode_string(&reply, "no_root");
				} else {
					const char *root_name = get_node_name(root);
					ei_x_encode_tuple_header(&reply, 2);
					ei_x_encode_atom(&reply, "reply");
					ei_x_encode_tuple_header(&reply, 3);
					ei_x_encode_atom(&reply, "object");
					ei_x_encode_string(&reply, root_name ? root_name : "root");
					ei_x_encode_long(&reply, (int64_t)root->get_instance_id());
				}
			}
		} else if (strcmp(function, "find_node") == 0) {
			// {call, godot, find_node, [NodePath]}
			if (args.size() < 1) {
				ei_x_encode_tuple_header(&reply, 2);
				ei_x_encode_atom(&reply, "error");
				ei_x_encode_string(&reply, "insufficient_args");
			} else {
				String path = args[0].operator String();
				inst = get_current_instance();
				if (inst == nullptr || inst->scene_tree == nullptr) {
					ei_x_encode_tuple_header(&reply, 2);
					ei_x_encode_atom(&reply, "error");
					ei_x_encode_string(&reply, "no_scene_tree");
				} else {
					Node *node = find_node_by_path(inst->scene_tree, path.utf8().get_data());
					if (node == nullptr) {
						ei_x_encode_tuple_header(&reply, 2);
						ei_x_encode_atom(&reply, "error");
						ei_x_encode_string(&reply, "node_not_found");
					} else {
						const char *node_name = get_node_name(node);
						ei_x_encode_tuple_header(&reply, 2);
						ei_x_encode_atom(&reply, "reply");
						ei_x_encode_tuple_header(&reply, 3);
						ei_x_encode_atom(&reply, "object");
						ei_x_encode_string(&reply, node_name ? node_name : "");
						ei_x_encode_long(&reply, (int64_t)node->get_instance_id());
					}
				}
			}
		} else {
			// Unknown function in godot module
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
	send_reply(&reply, fd);
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
		Variant args_array = bert_to_variant(buf, index);
		if (args_array.get_type() == Variant::ARRAY) {
			args = args_array.operator Array();
		}
	}

	// Route based on module (async, no reply)
	if (strcmp(module, "godot") == 0) {
		if (strcmp(function, "call_method") == 0) {
			// {cast, godot, call_method, [ObjectID, MethodName, Args]}
			if (args.size() >= 2) {
				int64_t object_id = args[0].operator int64_t();
				String method_name = args[1].operator String();

				// Guard: Check for valid object ID and method name
				if (object_id != 0 && !method_name.is_empty()) {
					Array method_args;
					if (args.size() > 2 && args[2].get_type() == Variant::ARRAY) {
						method_args = args[2].operator Array();
					}

					Object *obj = get_object_by_id(object_id);
					if (obj != nullptr) {
						obj->callv(method_name, method_args);
					}
				}
			}
		} else if (strcmp(function, "set_property") == 0) {
			// {cast, godot, set_property, [ObjectID, PropertyName, Value]}
			if (args.size() >= 3) {
				int64_t object_id = args[0].operator int64_t();
				String prop_name = args[1].operator String();
				Variant value = args[2];

				// Guard: Check for valid object ID and property name
				if (object_id != 0 && !prop_name.is_empty()) {
					Object *obj = get_object_by_id(object_id);
					if (obj != nullptr) {
						obj->set(prop_name, value);
					}
				}
			}
		}
	}

	return 0;
}

/*
 * Send reply to Erlang/Elixir
 */
static void send_reply(ei_x_buff *x, int fd) {
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

	/* Send encoded message to the connected node */
	if (ei_send_encoded(fd, NULL, x->buff, x->index) < 0) {
		fprintf(stderr, "Error sending reply\n");
	}
}

/*
 * Manual Erlang distribution handshake
 * Returns 0 on success, -1 on failure
 */
static int manual_handshake(int fd, ErlConnect *con) {
	unsigned char buf[1024];
	ssize_t n;
	int index = 0;
	int version;
	int type, arity;
	char atom[256];
	unsigned long challenge;
	unsigned char digest[MD5_DIGEST_LENGTH];
	MD5_CTX ctx;
	unsigned char cookie_digest[MD5_DIGEST_LENGTH];
	char status_msg[2];

	printf("Godot CNode: Starting manual handshake on fd: %d\n", fd);

	/* Step 1: Read length prefix (4 bytes, big-endian) */
	/* Note: Erlang distribution protocol uses length-prefixed messages */
	unsigned char len_buf[4];

	/* Use recv() instead of read() for socket operations */
	/* Wait a moment for the client to send data */
	fd_set read_fds;
	struct timeval timeout;
	FD_ZERO(&read_fds);
	FD_SET(fd, &read_fds);
	timeout.tv_sec = 5;
	timeout.tv_usec = 0;
	int select_result = select(fd + 1, &read_fds, NULL, NULL, &timeout);

	if (select_result <= 0) {
		fprintf(stderr, "Godot CNode: select() failed or timeout: %d, errno=%d (%s)\n", select_result, errno, strerror(errno));
		return -1;
	}

	/* Check socket error state */
	int socket_error = 0;
	socklen_t error_len = sizeof(socket_error);
	if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &error_len) == 0 && socket_error != 0) {
		fprintf(stderr, "Godot CNode: Socket error: %d (%s)\n", socket_error, strerror(socket_error));
		return -1;
	}

	printf("Godot CNode: Waiting for data from client (select returned %d)...\n", select_result);

	/* Try to peek at available data first */
	int available = 0;
	if (ioctl(fd, FIONREAD, &available) == 0) {
		printf("Godot CNode: Bytes available in socket buffer: %d\n", available);
	}

	n = recv(fd, len_buf, 4, MSG_PEEK); // Peek first to see what we have
	if (n > 0) {
		printf("Godot CNode: Peeked %zd bytes: 0x%02x 0x%02x 0x%02x 0x%02x\n", n, len_buf[0], len_buf[1], len_buf[2], len_buf[3]);
	}

	/* Now actually read the length prefix */
	n = recv(fd, len_buf, 4, 0);
	if (n != 4) {
		int saved_errno = errno;
		fprintf(stderr, "Godot CNode: Failed to recv length prefix: %zd bytes (expected 4), errno=%d (%s)\n", n, saved_errno, strerror(saved_errno));
		if (n == 0) {
			fprintf(stderr, "Godot CNode: Connection closed by peer (no data received)\n");
		} else if (n > 0) {
			fprintf(stderr, "Godot CNode: Partial read: got %zd bytes, showing first bytes: ", n);
			for (int i = 0; i < n && i < 4; i++) {
				fprintf(stderr, "0x%02x ", len_buf[i]);
			}
			fprintf(stderr, "\n");
		}
		return -1;
	}

	printf("Godot CNode: Received length prefix: 0x%02x 0x%02x 0x%02x 0x%02x\n", len_buf[0], len_buf[1], len_buf[2], len_buf[3]);
	uint32_t msg_len = (uint32_t)len_buf[0] << 24 | (uint32_t)len_buf[1] << 16 |
			(uint32_t)len_buf[2] << 8 | (uint32_t)len_buf[3];
	printf("Godot CNode: Name message length: %u bytes\n", msg_len);

	if (msg_len > sizeof(buf)) {
		fprintf(stderr, "Godot CNode: Message too large: %u bytes (max %zu)\n", msg_len, sizeof(buf));
		return -1;
	}

	/* Step 2: Read the actual name message */
	n = recv(fd, buf, msg_len, 0);
	if (n != (ssize_t)msg_len) {
		fprintf(stderr, "Godot CNode: Failed to recv name message: %zd bytes (expected %u), errno=%d (%s)\n", n, msg_len, errno, strerror(errno));
		return -1;
	}

	index = 0;
	if (ei_decode_version((const char *)buf, &index, &version) < 0) {
		fprintf(stderr, "Godot CNode: Failed to decode version\n");
		return -1;
	}

	if (buf[index] != 'n') {
		fprintf(stderr, "Godot CNode: Expected 'n' tag, got: 0x%02x\n", buf[index]);
		return -1;
	}
	index++;

	/* Read flags (1 byte) */
	unsigned char flags = buf[index++];

	/* Read node name */
	int name_len = 0;
	while (index < (int)msg_len && buf[index] != '\0' && name_len < sizeof(con->nodename) - 1) {
		con->nodename[name_len++] = buf[index++];
	}
	con->nodename[name_len] = '\0';

	printf("Godot CNode: Received name message: version=%d, flags=0x%02x, node=%s\n", version, flags, con->nodename);

	/* Step 3: Send status 'ok' with length prefix */
	status_msg[0] = 's';
	status_msg[1] = 'o'; // 'o' = ok, 'n' = nok
	uint32_t status_len = htonl(2);
	n = send(fd, &status_len, 4, 0);
	if (n != 4) {
		fprintf(stderr, "Godot CNode: Failed to send status length: %s\n", strerror(errno));
		return -1;
	}
	n = send(fd, status_msg, 2, 0);
	if (n != 2) {
		fprintf(stderr, "Godot CNode: Failed to send status: %s\n", strerror(errno));
		return -1;
	}

	/* Step 4: Generate and send challenge with length prefix */
	challenge = (unsigned long)time(NULL) ^ (unsigned long)getpid();
	unsigned char challenge_msg[5];
	challenge_msg[0] = 'n';
	*(unsigned long *)(challenge_msg + 1) = htonl((unsigned long)challenge);
	uint32_t challenge_len = htonl(5);
	n = send(fd, &challenge_len, 4, 0);
	if (n != 4) {
		fprintf(stderr, "Godot CNode: Failed to send challenge length: %s\n", strerror(errno));
		return -1;
	}
	n = send(fd, challenge_msg, 5, 0);
	if (n != 5) {
		fprintf(stderr, "Godot CNode: Failed to send challenge: %s\n", strerror(errno));
		return -1;
	}
	printf("Godot CNode: Sent challenge: %lu\n", challenge);

	/* Step 5: Receive challenge reply length prefix */
	n = recv(fd, len_buf, 4, 0);
	if (n != 4) {
		fprintf(stderr, "Godot CNode: Failed to recv challenge reply length: %zd bytes, errno=%d (%s)\n", n, errno, strerror(errno));
		return -1;
	}
	msg_len = (uint32_t)len_buf[0] << 24 | (uint32_t)len_buf[1] << 16 |
			(uint32_t)len_buf[2] << 8 | (uint32_t)len_buf[3];

	/* Step 6: Receive challenge reply */
	n = recv(fd, buf, msg_len, 0);
	if (n != (ssize_t)msg_len) {
		fprintf(stderr, "Godot CNode: Failed to recv challenge reply: %zd bytes (expected %u), errno=%d (%s)\n", n, msg_len, errno, strerror(errno));
		return -1;
	}
	if (msg_len < MD5_DIGEST_LENGTH + 1) {
		fprintf(stderr, "Godot CNode: Challenge reply too short: %u bytes\n", msg_len);
		return -1;
	}

	if (buf[0] != 'r') {
		fprintf(stderr, "Godot CNode: Expected 'r' tag, got: 0x%02x\n", buf[0]);
		return -1;
	}

	memcpy(digest, buf + 1, MD5_DIGEST_LENGTH);

	/* Step 7: Verify cookie */
	MD5_Init(&ctx);
	MD5_Update(&ctx, ec.ei_connect_cookie, strlen(ec.ei_connect_cookie));
	char challenge_str[32];
	snprintf(challenge_str, sizeof(challenge_str), "%lu", challenge);
	MD5_Update(&ctx, challenge_str, strlen(challenge_str));
	MD5_Final(cookie_digest, &ctx);

	if (memcmp(digest, cookie_digest, MD5_DIGEST_LENGTH) != 0) {
		fprintf(stderr, "Godot CNode: Cookie verification failed\n");
		return -1;
	}

	printf("Godot CNode: Cookie verified successfully\n");

	/* Step 8: Generate and send challenge acknowledgment with length prefix */
	unsigned long our_challenge = (unsigned long)time(NULL) ^ (unsigned long)getpid() ^ 0x12345678;
	MD5_Init(&ctx);
	MD5_Update(&ctx, ec.ei_connect_cookie, strlen(ec.ei_connect_cookie));
	snprintf(challenge_str, sizeof(challenge_str), "%lu", our_challenge);
	MD5_Update(&ctx, challenge_str, strlen(challenge_str));
	MD5_Final(cookie_digest, &ctx);

	unsigned char ack_msg[1 + MD5_DIGEST_LENGTH];
	ack_msg[0] = 'a';
	memcpy(ack_msg + 1, cookie_digest, MD5_DIGEST_LENGTH);
	uint32_t ack_len = htonl(1 + MD5_DIGEST_LENGTH);
	n = send(fd, &ack_len, 4, 0);
	if (n != 4) {
		fprintf(stderr, "Godot CNode: Failed to send challenge ack length: %s\n", strerror(errno));
		return -1;
	}
	n = send(fd, ack_msg, 1 + MD5_DIGEST_LENGTH, 0);
	if (n != 1 + MD5_DIGEST_LENGTH) {
		fprintf(stderr, "Godot CNode: Failed to send challenge ack: %s\n", strerror(errno));
		return -1;
	}

	printf("Godot CNode: Handshake completed successfully\n");
	return 0;
}
#pragma clang diagnostic pop

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
		/* Pass a valid ErlConnect structure (can be NULL, but some implementations prefer it) */
		static int accept_attempts = 0;
		accept_attempts++;
		if (accept_attempts == 1 || accept_attempts % 10 == 0) {
			printf("Godot CNode: Waiting for connection on listen_fd: %d (attempt %d)...\n", listen_fd, accept_attempts);

			/* Check socket state */
			int socket_error = 0;
			socklen_t error_len = sizeof(socket_error);
			if (getsockopt(listen_fd, SOL_SOCKET, SO_ERROR, &socket_error, &error_len) == 0) {
				if (socket_error != 0) {
					fprintf(stderr, "Godot CNode: Socket error: %d (%s)\n", socket_error, strerror(socket_error));
				} else {
					printf("Godot CNode: Socket state OK (no errors)\n");
				}
			}

			/* Check if socket is listening */
			int optval;
			socklen_t optlen = sizeof(optval);
			if (getsockopt(listen_fd, SOL_SOCKET, SO_ACCEPTCONN, &optval, &optlen) == 0) {
				printf("Godot CNode: Socket SO_ACCEPTCONN: %d\n", optval);
			}

			/* Get socket address info */
			struct sockaddr_in addr;
			socklen_t addr_len = sizeof(addr);
			if (getsockname(listen_fd, (struct sockaddr *)&addr, &addr_len) == 0) {
				char ip_str[INET_ADDRSTRLEN];
				inet_ntop(AF_INET, &addr.sin_addr, ip_str, INET_ADDRSTRLEN);
				printf("Godot CNode: Socket bound to %s:%d\n", ip_str, ntohs(addr.sin_port));
			}

			/* Check pending connections */
			int pending = 0;
			if (ioctl(listen_fd, FIONREAD, &pending) == 0) {
				printf("Godot CNode: Pending bytes in socket buffer: %d\n", pending);
			}
		}

		/* Use accept() instead of ei_accept() to work around macOS issue */
		/* ei_accept() fails with "Protocol not available" (errno 42) on macOS */
		if (accept_attempts == 1 || accept_attempts % 10 == 0) {
			printf("Godot CNode: Calling accept() (blocking, waiting for connection)...\n");
			fflush(stdout);
		}

		struct sockaddr_in client_addr;
		socklen_t client_addr_len = sizeof(client_addr);
		fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_addr_len);

		if (fd < 0) {
			int saved_errno = errno;
			fprintf(stderr, "Godot CNode: accept() failed: fd=%d, errno=%d (%s)\n", fd, saved_errno, strerror(saved_errno));

			/* Handle specific error codes */
			if (saved_errno == EBADF || saved_errno == 9) {
				/* Bad file descriptor - socket was closed */
				fprintf(stderr, "Godot CNode: listen_fd closed, exiting main loop\n");
				break;
			} else if (saved_errno == ECONNABORTED || saved_errno == 53) {
				/* Connection aborted - retry */
				if (accept_attempts % 10 == 0) {
					printf("Godot CNode: Connection aborted (errno: %d), retrying...\n", saved_errno);
				}
				continue;
			} else if (saved_errno == EINTR) {
				/* Interrupted by signal - retry */
				if (accept_attempts % 10 == 0) {
					printf("Godot CNode: accept() interrupted, retrying...\n");
				}
				continue;
			} else {
				/* Other error - log details and retry after short delay */
				fprintf(stderr, "Godot CNode: accept() error (errno: %d, %s), retrying after 100ms...\n", saved_errno, strerror(saved_errno));
				usleep(100000); // 100ms before retry
				continue;
			}
		}

		printf("Godot CNode: accept() succeeded: fd=%d\n", fd);

		/* Perform manual Erlang distribution handshake */
		ErlConnect con;
		memset(&con, 0, sizeof(ErlConnect));

		if (manual_handshake(fd, &con) < 0) {
			fprintf(stderr, "Godot CNode: Handshake failed, closing connection\n");
			close(fd);
			continue;
		}

		printf("Godot CNode: ✓ Accepted connection on fd: %d\n", fd);
		if (con.nodename[0] != '\0') {
			printf("Godot CNode: Connected from node: %s\n", con.nodename);
		} else {
			printf("Godot CNode: Connected from node: (nodename not provided)\n");
		}

		/* Receive message */
		printf("Godot CNode: Waiting to receive message from fd: %d...\n", fd);
		res = ei_receive_msg(fd, &msg, &x);
		printf("Godot CNode: ei_receive_msg returned: %d", res);

		if (res == ERL_TICK) {
			/* Just a tick, continue */
			printf(" (ERL_TICK - keepalive)\n");
			continue;
		} else if (res == ERL_ERROR) {
			fprintf(stderr, " (ERL_ERROR - errno: %d, %s)\n", errno, strerror(errno));
			close(fd);
			continue;
		} else {
			printf(" (success)\n");
		}

		printf("Godot CNode: Message type: %ld\n", msg.msgtype);

		/* Process the message */
		x.index = 0;
		if (process_message(x.buff, &x.index, fd) < 0) {
			fprintf(stderr, "Error processing message\n");
		}

		close(fd);
	}

	ei_x_free(&x);
}
} // extern "C"
