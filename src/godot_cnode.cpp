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

	/* Publish the node and get listen file descriptor */
	/* ei_publish registers with epmd AND creates a listening socket */
	/* The returned file descriptor IS the listening socket - use it directly */
	fd = ei_publish(&ec, 0);
	if (fd < 0) {
		/* If epmd is not running, try ei_listen as fallback */
		/* ei_listen creates a listening socket without epmd registration */
		if (errno == ECONNREFUSED || errno == 61) {
			fprintf(stderr, "ei_publish failed: epmd (Erlang Port Mapper Daemon) is not running\n");
			fprintf(stderr, "  Attempting fallback with ei_listen (node may not be discoverable)...\n");
			
			int port = 0;  // Let system choose port
			fd = ei_listen(&ec, &port, 5);  // backlog of 5
			if (fd < 0) {
				fprintf(stderr, "ei_listen also failed: %d (errno: %d, %s)\n", fd, errno, strerror(errno));
				fprintf(stderr, "  To fix: Start epmd with 'epmd -daemon' or ensure Erlang is installed\n");
				return -1;
			}
			fprintf(stderr, "  Successfully created listening socket on port %d (not registered with epmd)\n", port);
		} else {
			fprintf(stderr, "ei_publish failed: %d (errno: %d, %s)\n", fd, errno, strerror(errno));
			return -1;
		}
	} else {
		printf("Godot CNode: Successfully published node with epmd\n");
		printf("Godot CNode: Using listening socket from ei_publish (fd: %d)\n", fd);
	}

	listen_fd = fd;
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
		/* Use ei_accept_tmo with a timeout (5 seconds) to avoid blocking indefinitely */
		/* ei_accept_tmo returns immediately if no connection, ei_accept blocks forever */
		fd = ei_accept_tmo(&ec, listen_fd, NULL, 5000);  // 5 second timeout
		if (fd < 0) {
			/* Check if it's a timeout (expected when no connections) */
			if (errno == ETIMEDOUT || errno == 60) {
				/* Timeout is normal - just means no connection yet, continue waiting */
				continue;
			} else if (errno == EBADF || errno == 9) {
				/* Bad file descriptor - socket was closed */
				fprintf(stderr, "Godot CNode: listen_fd closed (errno: %d, %s)\n", errno, strerror(errno));
				break;
			} else {
				/* Other error - log but continue */
				fprintf(stderr, "ei_accept_tmo failed: %d (errno: %d, %s)\n", fd, errno, strerror(errno));
				usleep(100000);  // 100ms before retry
				continue;
			}
		}
		printf("Godot CNode: Accepted connection on fd: %d\n", fd);

		/* Receive message */
		res = ei_receive_msg(fd, &msg, &x);

		if (res == ERL_TICK) {
			/* Just a tick, continue */
			continue;
		} else if (res == ERL_ERROR) {
			fprintf(stderr, "Error receiving message: %d\n", res);
			close(fd);
			continue;
		}

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
