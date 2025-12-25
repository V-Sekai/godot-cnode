/*
 * Godot CNode - Erlang/Elixir CNode interface for Godot Engine (GDExtension)
 * 
 * This CNode allows Elixir/Erlang nodes to communicate with Godot
 * using the Erlang distribution protocol.
 * 
 * Adapted for GDExtension - works with current Godot instance
 */

// Define POSIX feature test macros BEFORE any includes to ensure timespec is defined
#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200112L

// Include C headers that define timespec BEFORE any C++ headers
#include <sys/types.h>
#include <time.h>
#include <sys/time.h>
#include <errno.h>
#include <cstring>

extern "C" {
#include "ei.h"
#include "ei_connect.h"
}

// Godot-cpp includes
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/main_loop.hpp>
#include <godot_cpp/variant/variant.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/node_path.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/core/object_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

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
    if (tree == nullptr) return nullptr;
    return tree->get_root();
}

/* Find node by path */
static Node *find_node_by_path(SceneTree *tree, const char *path_str) {
    if (tree == nullptr || path_str == nullptr) return nullptr;
    Node *root = tree->get_root();
    if (root == nullptr) return nullptr;
    NodePath path = NodePath(String::utf8(path_str));
    return root->get_node_or_null(path);
}

/* Get node name as string */
static const char *get_node_name(Node *node) {
    if (node == nullptr) return nullptr;
    String name = node->get_name();
    return name.utf8().get_data();
}

/* Get node by instance ID using godot-cpp */
static Node *get_node_by_id(int64_t node_id) {
    if (node_id == 0) return nullptr;
    // In godot-cpp, we use ObjectDB::get_instance()
    ObjectID obj_id = ObjectID((uint64_t)node_id);
    Object *obj = ObjectDB::get_instance(obj_id);
    if (obj == nullptr) return nullptr;
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
    
    /* Initialize ei library */
    res = ei_connect_init(&ec, nodename, cookie, 0);
    if (res < 0) {
        UtilityFunctions::printerr(String("ei_connect_init failed: ") + String::num(res));
        return -1;
    }
    
    /* Publish the node and get listen file descriptor */
    fd = ei_publish(&ec, 0);
    if (fd < 0) {
        UtilityFunctions::printerr(String("ei_publish failed: ") + String::num(fd));
        return -1;
    }
    
    listen_fd = fd;
    return 0;
}
} // extern "C"

/*
 * Process incoming message from Erlang/Elixir
 */
static int process_message(char *buf, int *index, int fd) {
    int arity;
    char atom[MAXATOMLEN];
    
    /* Decode the message */
    if (ei_decode_version(buf, index, NULL) < 0) {
        UtilityFunctions::printerr("Error decoding version");
        return -1;
    }
    
    if (ei_decode_tuple_header(buf, index, &arity) < 0) {
        UtilityFunctions::printerr("Error decoding tuple header");
        return -1;
    }
    
    /* Get the message type atom */
    if (ei_decode_atom(buf, index, atom) < 0) {
        UtilityFunctions::printerr("Error decoding atom");
        return -1;
    }
    
    /* Handle different message types */
    if (strcmp(atom, "call") == 0) {
        return handle_call(buf, index, fd);
    } else if (strcmp(atom, "cast") == 0) {
        return handle_cast(buf, index);
    } else {
        UtilityFunctions::printerr(String("Unknown message type: ") + atom);
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

/*
 * Handle synchronous call from Erlang/Elixir
 */
static int handle_call(char *buf, int *index, int fd) {
    char atom[MAXATOMLEN];
    ei_x_buff reply;
    long instance_id;
    godot_instance_t *inst;
    
    /* Get the function name */
    if (ei_decode_atom(buf, index, atom) < 0) {
        UtilityFunctions::printerr("Error decoding function atom");
        return -1;
    }
    
    /* Initialize reply buffer */
    ei_x_new(&reply);
    
    /* Handle different function calls */
    if (strcmp(atom, "ping") == 0) {
        ei_x_encode_tuple_header(&reply, 2);
        ei_x_encode_atom(&reply, "reply");
        ei_x_encode_atom(&reply, "pong");
    } else if (strcmp(atom, "godot_version") == 0) {
        ei_x_encode_tuple_header(&reply, 2);
        ei_x_encode_atom(&reply, "reply");
        ei_x_encode_string(&reply, "4.1");
    } else if (strcmp(atom, "get_scene_tree_root") == 0) {
        /* Get the root node of the scene tree */
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
                ei_x_encode_tuple_header(&reply, 2);
                ei_x_encode_string(&reply, root_name ? root_name : "root");
                ei_x_encode_long(&reply, (int64_t)root->get_instance_id());
            }
        }
    } else if (strcmp(atom, "find_node") == 0) {
        /* Find a node by path */
        char path[256];
        if (ei_decode_string(buf, index, path) < 0) {
            ei_x_encode_tuple_header(&reply, 2);
            ei_x_encode_atom(&reply, "error");
            ei_x_encode_string(&reply, "invalid_path");
        } else {
            inst = get_current_instance();
            if (inst == nullptr || inst->scene_tree == nullptr) {
                ei_x_encode_tuple_header(&reply, 2);
                ei_x_encode_atom(&reply, "error");
                ei_x_encode_string(&reply, "no_scene_tree");
            } else {
                Node *node = find_node_by_path(inst->scene_tree, path);
                if (node == nullptr) {
                    ei_x_encode_tuple_header(&reply, 2);
                    ei_x_encode_atom(&reply, "error");
                    ei_x_encode_string(&reply, "node_not_found");
                } else {
                    const char *node_name = get_node_name(node);
                    ei_x_encode_tuple_header(&reply, 2);
                    ei_x_encode_atom(&reply, "reply");
                    ei_x_encode_tuple_header(&reply, 2);
                    ei_x_encode_string(&reply, node_name ? node_name : "");
                    ei_x_encode_long(&reply, (int64_t)node->get_instance_id());
                }
            }
        }
    } else if (strcmp(atom, "get_node_property") == 0) {
        /* Get a node property */
        long node_id;
        char prop_name[256];
        if (ei_decode_long(buf, index, &node_id) < 0 || ei_decode_string(buf, index, prop_name) < 0) {
            ei_x_encode_tuple_header(&reply, 2);
            ei_x_encode_atom(&reply, "error");
            ei_x_encode_string(&reply, "invalid_args");
        } else {
            Node *node = get_node_by_id(node_id);
            if (node == nullptr) {
                ei_x_encode_tuple_header(&reply, 2);
                ei_x_encode_atom(&reply, "error");
                ei_x_encode_string(&reply, "node_not_found");
            } else {
                String prop = String::utf8(prop_name);
                Variant value = node->get(prop);
                ei_x_encode_tuple_header(&reply, 2);
                ei_x_encode_atom(&reply, "reply");
                variant_to_bert(value, &reply);
            }
        }
    } else if (strcmp(atom, "set_node_property") == 0) {
        /* Set a node property */
        long node_id;
        char prop_name[256];
        if (ei_decode_long(buf, index, &node_id) < 0 || ei_decode_string(buf, index, prop_name) < 0) {
            ei_x_encode_tuple_header(&reply, 2);
            ei_x_encode_atom(&reply, "error");
            ei_x_encode_string(&reply, "invalid_args");
        } else {
            Variant value = bert_to_variant(buf, index);
            Node *node = get_node_by_id(node_id);
            if (node == nullptr) {
                ei_x_encode_tuple_header(&reply, 2);
                ei_x_encode_atom(&reply, "error");
                ei_x_encode_string(&reply, "node_not_found");
            } else {
                String prop = String::utf8(prop_name);
                node->set(prop, value);
                ei_x_encode_tuple_header(&reply, 2);
                ei_x_encode_atom(&reply, "reply");
                ei_x_encode_atom(&reply, "ok");
            }
        }
    } else if (strcmp(atom, "call_node_method") == 0) {
        /* Call a node method */
        long node_id;
        char method_name[256];
        if (ei_decode_long(buf, index, &node_id) < 0 || ei_decode_string(buf, index, method_name) < 0) {
            ei_x_encode_tuple_header(&reply, 2);
            ei_x_encode_atom(&reply, "error");
            ei_x_encode_string(&reply, "invalid_args");
        } else {
            // Decode arguments array
            Variant args_array = bert_to_variant(buf, index);
            Array args;
            if (args_array.get_type() == Variant::ARRAY) {
                args = args_array.operator Array();
            }
            
            Node *node = get_node_by_id(node_id);
            if (node == nullptr) {
                ei_x_encode_tuple_header(&reply, 2);
                ei_x_encode_atom(&reply, "error");
                ei_x_encode_string(&reply, "node_not_found");
            } else {
                String method = String::utf8(method_name);
                Variant result = node->callv(method, args);
                ei_x_encode_tuple_header(&reply, 2);
                ei_x_encode_atom(&reply, "reply");
                variant_to_bert(result, &reply);
            }
        }
    } else {
        /* Unknown function */
        ei_x_encode_tuple_header(&reply, 2);
        ei_x_encode_atom(&reply, "error");
        ei_x_encode_string(&reply, "unknown_function");
    }
    
    /* Send reply */
    send_reply(&reply, fd);
    ei_x_free(&reply);
    
    return 0;
}

/*
 * Handle asynchronous cast from Erlang/Elixir
 */
static int handle_cast(char *buf, int *index) {
    char atom[MAXATOMLEN];
    
    /* Get the function name */
    if (ei_decode_atom(buf, index, atom) < 0) {
        UtilityFunctions::printerr("Error decoding function atom");
        return -1;
    }
    
    /* Handle different cast functions */
    if (strcmp(atom, "log") == 0) {
        char msg[256];
        if (ei_decode_string(buf, index, msg) == 0) {
            UtilityFunctions::print(String("[Godot CNode] ") + msg);
        }
    }
    
    return 0;
}

/*
 * Send reply to Erlang/Elixir
 */
static void send_reply(ei_x_buff *x, int fd) {
    /* Send encoded message to the connected node */
    if (ei_send_encoded(fd, NULL, x->buff, x->index) < 0) {
        UtilityFunctions::printerr("Error sending reply");
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
    
    UtilityFunctions::print("Godot CNode: Entering main loop");
    
    ei_x_new(&x);
    
    while (1) {
        /* Accept connection from Erlang/Elixir node */
        fd = ei_accept(&ec, listen_fd, NULL);
        if (fd < 0) {
            UtilityFunctions::printerr(String("ei_accept failed: ") + String::num(fd));
            break;
        }
        
        /* Receive message */
        res = ei_receive_msg(fd, &msg, &x);
        
        if (res == ERL_TICK) {
            /* Just a tick, continue */
            continue;
        } else if (res == ERL_ERROR) {
            UtilityFunctions::printerr(String("Error receiving message: ") + String::num(res));
            close(fd);
            continue;
        }
        
        /* Process the message */
        x.index = 0;
        if (process_message(x.buff, &x.index, fd) < 0) {
            UtilityFunctions::printerr("Error processing message");
        }
        
        close(fd);
    }
    
    ei_x_free(&x);
}
} // extern "C"
