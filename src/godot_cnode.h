#pragma once

#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/variant/variant.hpp>
#include <godot_cpp/core/object.hpp>

#ifdef __cplusplus
extern "C" {
#endif

// Forward declarations for CNode functions and variables
#define MAX_INSTANCES 16

// In GDExtension, we work with the current Godot instance, not create new ones
// The instances array is simplified - we just track the current SceneTree
typedef struct {
    int id;
    godot::SceneTree *scene_tree;  // Reference to current SceneTree
    int started;
} godot_instance_t;

extern godot_instance_t instances[MAX_INSTANCES];
extern int listen_fd;

int init_cnode(char *nodename, char *cookie);
void main_loop(void);

#ifdef __cplusplus
}
#endif

