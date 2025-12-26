#pragma once

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/variant.hpp>

#ifdef __cplusplus
extern "C" {
#endif

// Forward declarations for CNode functions and variables
#define MAX_INSTANCES 16

// In GDExtension, we work with the current Godot instance, not create new ones
// The instances array is simplified - we just track the current SceneTree
typedef struct {
	int id;
	godot::SceneTree *scene_tree; // Reference to current SceneTree
	int started;
} godot_instance_t;

extern godot_instance_t instances[MAX_INSTANCES];
extern int listen_fd;
extern int next_instance_id;

int init_cnode(char *nodename, char *cookie);
void main_loop(void);
int process_cnode_frame(void); // Non-blocking version for main thread

#ifdef __cplusplus
}

// CNodeServer Node class - runs on main thread
namespace godot {
class CNodeServer : public Node {
	GDCLASS(CNodeServer, Node);

private:
	bool initialized;
	char *cookie_copy;

protected:
	static void _bind_methods();

public:
	CNodeServer();
	~CNodeServer();

	void _ready() override;
	void _process(double delta) override;

	// Called deferred to add node to scene tree
	void _add_to_scene_tree();
};
} // namespace godot

#endif
