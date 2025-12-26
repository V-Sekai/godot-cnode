#include "register_types.h"
#include "godot_cnode.h"

#include <godot_cpp/classes/crypto.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/window.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <cstdio>
#include <cstring>
#ifndef _WIN32
#include <unistd.h>
#endif

// CNode server node management (runs on main thread)
static godot::Node *cnode_server_node = nullptr;

// Forward declarations from godot_cnode.cpp
extern "C" {
extern godot_instance_t instances[MAX_INSTANCES];
extern int listen_fd;
extern int next_instance_id;
int init_cnode(char *nodename, char *cookie);
void main_loop(void);
}

// CNodeServer class is defined in godot_cnode.h

// Generate a cryptographically secure random string using Godot's Crypto class
// Must be called on the main thread
static String generate_cryptorandom_string(int length = 32) {
	using namespace godot;

	// Erlang cookies typically use ASCII alphanumeric characters
	const char charset[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
	const int charset_size = sizeof(charset) - 1; // -1 to exclude null terminator

	// Use Godot's Crypto class to generate random bytes
	Object *crypto_obj = ClassDB::instantiate("Crypto");
	if (crypto_obj == nullptr) {
		UtilityFunctions::printerr("Godot CNode: Failed to create Crypto instance");
		return "godotcookie"; // Fallback cookie
	}

	Ref<Crypto> crypto = Object::cast_to<Crypto>(crypto_obj);
	if (crypto.is_null()) {
		// If cast fails, we need to delete the object manually
		memdelete(crypto_obj);
		UtilityFunctions::printerr("Godot CNode: Failed to cast to Crypto");
		return "godotcookie"; // Fallback cookie
	}
	// Ref<> now owns the object, no need to delete manually

	// Generate random bytes
	PackedByteArray random_bytes = crypto->generate_random_bytes(length);
	if (random_bytes.size() != length) {
		UtilityFunctions::printerr("Godot CNode: Failed to generate random bytes");
		return "godotcookie"; // Fallback cookie
	}

	// Convert random bytes to string using charset
	String result;
	for (int i = 0; i < length; i++) {
		uint8_t byte = random_bytes[i];
		result += charset[byte % charset_size];
	}

	return result;
}

// Helper function to read or generate cookie from Godot user data directory
// Must be called on the main thread
static String read_or_generate_godot_cnode_cookie() {
	using namespace godot;

	// Priority 1: Environment variable override
	OS *os = OS::get_singleton();
	if (os) {
		String env_cookie = os->get_environment("GODOT_CNODE_COOKIE");
		if (!env_cookie.is_empty()) {
			UtilityFunctions::print("Godot CNode: Using cookie from GODOT_CNODE_COOKIE environment variable");
			return env_cookie.strip_edges();
		}
	}

	// Need OS singleton for user data directory
	if (!os) {
		UtilityFunctions::printerr("Godot CNode: OS singleton not available");
		return "godotcookie"; // Fallback cookie
	}

	// Priority 2: User data directory (project-specific)
	String user_data_dir = os->get_user_data_dir();
	String cookie_path = user_data_dir.path_join("cnode_cookie");

	Ref<FileAccess> file = FileAccess::open(cookie_path, FileAccess::READ);
	if (file.is_valid()) {
		String cookie = file->get_as_text().strip_edges();
		file->close();
		if (!cookie.is_empty()) {
			UtilityFunctions::print(String("Godot CNode: Using cookie from user data directory: ") + cookie_path);
			return cookie;
		}
	}

	// Priority 3: Generate a new cryptographically random cookie using Godot's Crypto
	UtilityFunctions::print("Godot CNode: No cookie file found, generating new cryptographically random cookie...");
	String new_cookie = generate_cryptorandom_string(32);

	// Save the generated cookie to the user data directory
	file = FileAccess::open(cookie_path, FileAccess::WRITE);
	if (file.is_valid()) {
		file->store_string(new_cookie);
		file->close();

		UtilityFunctions::print(String("Godot CNode: Generated and saved new cookie to: ") + cookie_path);
		UtilityFunctions::print(String("  Cookie: ") + new_cookie);
	} else {
		UtilityFunctions::printerr(String("Godot CNode: Failed to save cookie to: ") + cookie_path);
		UtilityFunctions::printerr("  Using generated cookie for this session only");
	}

	return new_cookie;
}

void initialize_cnode_module(ModuleInitializationLevel p_level) {
	using namespace godot;

	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}

	// Register CNodeServer class
	ClassDB::register_class<CNodeServer>();

	// Create CNodeServer node
	CNodeServer *cnode_server = memnew(CNodeServer);
	cnode_server->set_name("CNodeServer");
	cnode_server_node = cnode_server;

	// Use Engine's call_deferred to add node when scene tree is ready
	// This ensures the scene tree is fully initialized
	Engine *engine = Engine::get_singleton();
	if (engine != nullptr) {
		// Use call_deferred with method name string
		cnode_server->call_deferred("_add_to_scene_tree");
		UtilityFunctions::print("Godot CNode: CNodeServer node created, will be added to scene tree deferred");
	} else {
		UtilityFunctions::printerr("Godot CNode: Engine singleton not available");
		memdelete(cnode_server);
		cnode_server_node = nullptr;
	}
}

void uninitialize_cnode_module(ModuleInitializationLevel p_level) {
	using namespace godot;

	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}

	// Clean up CNodeServer node
	// During shutdown, be very careful - Godot objects may already be destroyed
	// Avoid calling Godot API functions during shutdown as they may crash

	// Close listen_fd to stop accepting connections (safe - just a file descriptor)
	if (listen_fd >= 0) {
		close(listen_fd);
		listen_fd = -1;
	}

	// During shutdown, Godot will automatically clean up nodes in the scene tree
	// We should NOT try to manually delete nodes during shutdown as the scene tree
	// may already be destroyed, causing crashes
	// Just clear the pointer - let Godot handle the cleanup
	if (cnode_server_node != nullptr) {
		cnode_server_node = nullptr;
	}
}

extern "C" {

GDExtensionBool GDE_EXPORT godot_cnode_library_init(
		const GDExtensionInterfaceGetProcAddress p_get_proc_address,
		GDExtensionClassLibraryPtr p_library,
		GDExtensionInitialization *r_initialization) {
	godot::GDExtensionBinding::InitObject init_obj(p_get_proc_address, p_library, r_initialization);

	init_obj.register_initializer(initialize_cnode_module);
	init_obj.register_terminator(uninitialize_cnode_module);
	init_obj.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);

	return init_obj.init();
}
}
