#include "register_types.h"
#include "godot_cnode.h"

#include <godot_cpp/classes/crypto.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#ifndef _WIN32
#include <pthread.h>
#include <unistd.h>
#else
#include <process.h>
#include <windows.h>
typedef HANDLE pthread_t;
#endif
#include <cstdio>
#include <cstring>

// CNode server thread management
static pthread_t cnode_thread = 0;
static bool cnode_running = false;
// Store cookie as C string to avoid Godot API initialization issues
static char cnode_cookie[256] = { 0 }; // Cookie prepared on main thread, stored as C string

// Forward declarations from godot_cnode.cpp
extern "C" {
extern godot_instance_t instances[MAX_INSTANCES];
extern int listen_fd;
extern int next_instance_id;
int init_cnode(char *nodename, char *cookie);
void main_loop(void);
}

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

// CNode server thread function (NO Godot API calls allowed here!)
static void *cnode_server_thread_impl(void *userdata) {
	// Get cookie from userdata (plain C string passed as void*)
	const char *cookie = static_cast<const char *>(userdata);

	// Initialize instances array
	memset(instances, 0, sizeof(instances));

	// Use localhost first since it's more reliable than .local hostnames
	// .local hostnames (mDNS) may not resolve correctly in all network configurations
	printf("Godot CNode GDExtension: Starting server thread...\n");
	printf("  Cookie: %s\n", cookie);

	// Try different hostname options in order of preference
	// Use 127.0.0.1 first since Erlang requires fully qualified hostnames
	// and "localhost" may be rejected by some Erlang configurations
	char hostname[256] = { 0 };
	char nodename[512] = { 0 };
	const char *nodename_options[4]; // Max 4 options
	int option_count = 0;

	// Use 127.0.0.1 first (fully qualified, always works)
	nodename_options[option_count++] = "godot@127.0.0.1";
	// Also try localhost (may not work if Erlang requires FQDN)
	nodename_options[option_count++] = "godot@localhost";

	// Also try actual hostname if it's not .local (which may not resolve)
#ifndef _WIN32
	if (gethostname(hostname, sizeof(hostname) - 1) == 0) {
		// Only use hostname if it doesn't end in .local (which may not resolve)
		if (strlen(hostname) > 6 && strcmp(hostname + strlen(hostname) - 6, ".local") != 0) {
			snprintf(nodename, sizeof(nodename), "godot@%s", hostname);
			nodename_options[option_count++] = nodename;
		}
	}
#endif
	nodename_options[option_count] = nullptr;

	printf("  Trying node names in order:");
	for (int i = 0; i < option_count; i++) {
		printf(" %s", nodename_options[i]);
		if (i < option_count - 1)
			printf(",");
	}
	printf("\n");

	bool initialized = false;
	for (int i = 0; nodename_options[i] != nullptr; i++) {
		if (init_cnode(const_cast<char *>(nodename_options[i]), const_cast<char *>(cookie)) == 0) {
			initialized = true;
			if (i > 0) {
				printf("Godot CNode: Successfully initialized with %s (fallback option %d)\n", nodename_options[i], i);
			}
			break;
		} else {
			fprintf(stderr, "Failed to initialize CNode with %s", nodename_options[i]);
			if (nodename_options[i + 1] != nullptr) {
				fprintf(stderr, ", trying next option...\n");
			} else {
				fprintf(stderr, "\n");
			}
		}
	}

	if (!initialized) {
		fprintf(stderr, "Failed to initialize CNode with all hostname options\n");
		cnode_running = false;
		delete[] static_cast<char *>(userdata); // Clean up cookie string
		return nullptr;
	}

	printf("Godot CNode GDExtension: Published and ready (listen_fd: %d)\n", listen_fd);

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

	delete[] static_cast<char *>(userdata); // Clean up cookie string
	return nullptr;
}

#ifdef _WIN32
// Windows thread wrapper
static unsigned __stdcall cnode_server_thread_win(void *userdata) {
	cnode_server_thread_impl(userdata);
	return 0;
}
#else
// Unix thread function (pthread)
static void *cnode_server_thread(void *userdata) {
	return cnode_server_thread_impl(userdata);
}
#endif

void initialize_cnode_module(ModuleInitializationLevel p_level) {
	// Don't use ANY Godot APIs during initialization - they may crash
	// We'll start the server later when it's safe to use Godot APIs
	// For now, just use a default cookie from environment variable (C standard library only)

	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}

	// Use only C standard library - no Godot APIs!
	// Try to read cookie from environment variable
	const char *env_cookie = getenv("GODOT_CNODE_COOKIE");
	if (env_cookie && strlen(env_cookie) > 0) {
		size_t len = strlen(env_cookie);
		if (len >= sizeof(cnode_cookie)) {
			len = sizeof(cnode_cookie) - 1;
		}
		memcpy(cnode_cookie, env_cookie, len);
		cnode_cookie[len] = '\0';
	} else {
		// Use default cookie - will be replaced later when Godot APIs are available
		strncpy(cnode_cookie, "godotcookie", sizeof(cnode_cookie) - 1);
		cnode_cookie[sizeof(cnode_cookie) - 1] = '\0';
	}

	// Start server with the cookie we have (no Godot API calls)
	if (!cnode_running && cnode_thread == 0) {
		// Create a copy of the cookie for the thread
		size_t cookie_len = strlen(cnode_cookie);
		char *cookie_copy = new char[cookie_len + 1];
		memcpy(cookie_copy, cnode_cookie, cookie_len + 1);

#ifndef _WIN32
		// Use pthread on Unix-like systems
		if (pthread_create(&cnode_thread, nullptr, cnode_server_thread, cookie_copy) != 0) {
			delete[] cookie_copy;
			return;
		}
		// Thread will be joined in uninitialize_cnode_module
#else
		// Windows: use _beginthreadex
		unsigned int thread_id;
		cnode_thread = (pthread_t)_beginthreadex(nullptr, 0, cnode_server_thread_win, cookie_copy, 0, &thread_id);
		if (cnode_thread == 0) {
			delete[] cookie_copy;
			return;
		}
#endif
		// Server started successfully (using printf since we can't use Godot APIs yet)
		printf("Godot CNode GDExtension: Server thread started (using cookie from environment or default)\n");
	}
}

void uninitialize_cnode_module(ModuleInitializationLevel p_level) {
	using namespace godot;

	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}

	// Stop CNode server
	if (cnode_running && cnode_thread != 0) {
		UtilityFunctions::print("Godot CNode GDExtension: Stopping server thread...");
		// Close listen_fd to break out of main_loop
		if (listen_fd >= 0) {
			close(listen_fd);
			listen_fd = -1;
		}

		// Wait for thread to finish
#ifndef _WIN32
		if (cnode_thread != 0) {
			pthread_join(cnode_thread, nullptr);
			cnode_thread = 0;
		}
#else
		if (cnode_thread != 0) {
			WaitForSingleObject(cnode_thread, INFINITE);
			CloseHandle(cnode_thread);
			cnode_thread = 0;
		}
#endif
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
