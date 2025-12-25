#include "register_types.h"
#include "godot_cnode.h"

#include <godot_cpp/classes/crypto.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/core/memory.hpp>

#ifndef _WIN32
#include <unistd.h>
#endif
#include <atomic>
#include <cstring>
#include <thread>

// CNode server thread management
static std::thread cnode_thread;
static std::atomic<bool> cnode_running(false);

// Forward declarations from godot_cnode.cpp
extern "C" {
extern godot_instance_t instances[MAX_INSTANCES];
extern int listen_fd;
extern int next_instance_id;
int init_cnode(char *nodename, char *cookie);
void main_loop(void);
}

// Generate a cryptographically secure random string using Godot's Crypto class
static String generate_cryptorandom_string(int length = 32) {
	using namespace godot;
	
	// Erlang cookies typically use ASCII alphanumeric characters
	const char charset[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
	const int charset_size = sizeof(charset) - 1; // -1 to exclude null terminator
	
	// Use Godot's Crypto class to generate random bytes
	// Crypto is RefCounted, so we use ClassDB::instantiate and cast to Ref<Crypto>
	Object *crypto_obj = ClassDB::instantiate("Crypto");
	if (crypto_obj == nullptr) {
		UtilityFunctions::printerr("Godot CNode: Failed to create Crypto instance");
		return "godotcookie"; // Fallback
	}
	
	Ref<Crypto> crypto = Object::cast_to<Crypto>(crypto_obj);
	if (crypto.is_null()) {
		UtilityFunctions::printerr("Godot CNode: Failed to cast to Crypto");
		// If cast fails, we need to delete the object manually
		memdelete(crypto_obj);
		return "godotcookie"; // Fallback
	}
	// Ref<> now owns the object, no need to delete manually
	
	// Generate random bytes
	PackedByteArray random_bytes = crypto->generate_random_bytes(length);
	if (random_bytes.size() != length) {
		UtilityFunctions::printerr("Godot CNode: Failed to generate random bytes");
		return "godotcookie"; // Fallback
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
static String read_or_generate_godot_cnode_cookie() {
	using namespace godot;
	
	OS *os = OS::get_singleton();
	if (!os) {
		UtilityFunctions::printerr("Godot CNode: OS singleton not available");
		return "godotcookie"; // Fallback
	}
	
	// Priority 1: Environment variable override
	String env_cookie = os->get_environment("GODOT_CNODE_COOKIE");
	if (!env_cookie.is_empty()) {
		UtilityFunctions::print("Godot CNode: Using cookie from GODOT_CNODE_COOKIE environment variable");
		return env_cookie.strip_edges();
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

// CNode server thread function
static void cnode_server_thread() {
	using namespace godot;
	
	// Read or generate cookie from Godot user data directory
	String cookie_str = read_or_generate_godot_cnode_cookie();
	
	// Convert to C string (CharString manages the memory)
	CharString cookie_cstr = cookie_str.utf8();
	const char *cookie = cookie_cstr.get_data();
	
	// Node name (could also be made configurable)
	char *nodename = const_cast<char *>("godot@127.0.0.1");

	// Initialize instances array
	memset(instances, 0, sizeof(instances));

	UtilityFunctions::print("Godot CNode GDExtension: Starting server thread...");
	UtilityFunctions::print(String("  Node: ") + nodename);
	UtilityFunctions::print(String("  Cookie: ") + cookie_str);

	// Initialize CNode
	if (init_cnode(nodename, const_cast<char *>(cookie)) < 0) {
		UtilityFunctions::printerr("Failed to initialize CNode");
		cnode_running = false;
		return;
	}

	UtilityFunctions::print(String("Godot CNode GDExtension: Published and ready (listen_fd: ") + String::num(listen_fd) + ")");

	cnode_running = true;

	// Enter main loop
	main_loop();

	cnode_running = false;

	// Cleanup: clear all instances
	for (int i = 0; i < MAX_INSTANCES; i++) {
		if (instances[i].id != 0) {
			// Note: In GDExtension, we don't destroy Godot instances
			// The instances array is just for tracking
			instances[i].id = 0;
			instances[i].scene_tree = nullptr;
			instances[i].started = 0;
		}
	}
}

void initialize_cnode_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}

	// Start CNode server in background thread
	if (!cnode_running && !cnode_thread.joinable()) {
		cnode_thread = std::thread(cnode_server_thread);
		UtilityFunctions::print("Godot CNode GDExtension: Server thread started");
	}
}

void uninitialize_cnode_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}

	// Stop CNode server
	if (cnode_running && cnode_thread.joinable()) {
		UtilityFunctions::print("Godot CNode GDExtension: Stopping server thread...");
		// Close listen_fd to break out of main_loop
		if (listen_fd >= 0) {
			close(listen_fd);
			listen_fd = -1;
		}

		// Wait for thread to finish
		cnode_thread.join();
		UtilityFunctions::print("Godot CNode GDExtension: Server thread stopped");
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

