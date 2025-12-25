# Godot CNode GDExtension

Erlang/Elixir CNode interface for Godot Engine as a GDExtension.

This GDExtension allows Erlang/Elixir nodes to communicate with Godot using the Erlang distribution protocol. The CNode server automatically starts when the GDExtension is loaded.

## Building

### Prerequisites

- Godot 4.1+ source code (for godot-cpp)
- Erlang/OTP (for erl_interface headers)
- SCons

### Build Steps

1. Clone godot-cpp as a submodule or copy it to `thirdparty/godot-cpp`:
   ```bash
   git submodule add https://github.com/godotengine/godot-cpp.git thirdparty/godot-cpp
   ```

2. Copy erl_interface source to `thirdparty/erl_interface`:
   ```bash
   # Extract erl_interface from Erlang/OTP source
   cp -r /path/to/otp/lib/erl_interface thirdparty/
   ```

3. Build the GDExtension:
   ```bash
   scons target=template_release
   ```

## Usage

1. Copy the `bin/addons/godot_cnode` directory to your Godot project's `addons/` folder
2. Enable the GDExtension in your Godot project
3. The CNode server will automatically start when Godot loads the GDExtension
4. Connect from Erlang/Elixir:
   ```elixir
   Node.connect(:"godot@127.0.0.1")
   ```

## Architecture

The CNode runs in a background thread and communicates with the current Godot instance. It provides RPC functions to:
- Access the SceneTree
- Find nodes by path
- Get/set node properties
- Call node methods
- Discover Godot's API dynamically using ClassDB
- Access singletons
- Work with any Object (not just Nodes)
- Convert between Godot Variants and Erlang BERT format

The API discovery system is inspired by the godot-sandbox project's runtime API generation, but adapted for Erlang/Elixir communication over CNode instead of sandboxed programs.

## API

### Basic RPC Calls

- `ping` - Returns "pong" for connectivity testing
- `godot_version` - Returns Godot version string

### Scene Tree & Node Operations

- `get_scene_tree_root` - Returns the root node of the scene tree
- `find_node` - Finds a node by path string
- `get_node_property` - Gets a property value from a node
- `set_node_property` - Sets a property value on a node
- `call_node_method` - Calls a method on a node with arguments

### ClassDB API Discovery

- `list_classes` - Returns a list of all registered Godot classes
- `get_class_methods` - Returns methods for a given class name
- `get_class_properties` - Returns properties for a given class name

### Singleton Access

- `get_singletons` - Returns a list of all singleton names
- `get_singleton` - Gets a singleton object by name

### Generic Object Operations

- `call_object_method` - Calls a method on any Object (not just Node) by instance ID
- `get_object_property` - Gets a property from any Object by instance ID
- `set_object_property` - Sets a property on any Object by instance ID

## License

Same as Godot Engine (MIT)
