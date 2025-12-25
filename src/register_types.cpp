#include "register_types.h"
#include "godot_cnode.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <thread>
#include <atomic>
#include <cstring>
#include <unistd.h>

// CNode server thread management
static std::thread cnode_thread;
static std::atomic<bool> cnode_running(false);

// Forward declarations from godot_cnode.cpp
extern "C" {
    extern godot_instance_t instances[MAX_INSTANCES];
    extern int listen_fd;
    int init_cnode(char *nodename, char *cookie);
    void main_loop(void);
}

// CNode server thread function
static void cnode_server_thread() {
    char *nodename = const_cast<char*>("godot@127.0.0.1");
    char *cookie = const_cast<char*>("godotcookie");
    
    // Initialize instances array
    memset(instances, 0, sizeof(instances));
    
    UtilityFunctions::print("Godot CNode GDExtension: Starting server thread...");
    UtilityFunctions::print(String("  Node: ") + nodename);
    UtilityFunctions::print(String("  Cookie: ") + cookie);
    
    // Initialize CNode
    if (init_cnode(nodename, cookie) < 0) {
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
    GDExtensionInitialization *r_initialization
) {
    godot::GDExtensionBinding::InitObject init_obj(p_get_proc_address, p_library, r_initialization);

    init_obj.register_initializer(initialize_cnode_module);
    init_obj.register_terminator(uninitialize_cnode_module);
    init_obj.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);

    return init_obj.init();
}

}
