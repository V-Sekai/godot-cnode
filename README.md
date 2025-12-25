# Godot CNode GDExtension

Erlang/Elixir CNode interface for Godot Engine as a GDExtension.

This GDExtension allows Erlang/Elixir nodes to communicate with Godot using the Erlang distribution protocol. The CNode server automatically starts when the GDExtension is loaded.

## Supported Platforms

- **Linux** (x86_64)
- **macOS** (universal)
- **Windows** (x86_32, x86_64)
- **iOS** (arm64)
- **Android** (x86_32, x86_64, arm32, arm64)

Note: Web platform is not supported. This extension requires Erlang's native distribution protocol via `erl_interface`.

## Building

### Prerequisites

- Godot 4.1+ source code (for godot-cpp)
- Erlang/OTP with erl_interface development libraries installed:
  - **Linux**: `sudo apt-get install erlang-dev` (Debian/Ubuntu) or equivalent
  - **macOS**: `brew install erlang` (Homebrew)
  - **Windows**: Install Erlang/OTP from [erlang.org](https://www.erlang.org/downloads)
- SCons

### Build Steps

1. Clone godot-cpp as a submodule or copy it to `thirdparty/godot-cpp`:
   ```bash
   git submodule add https://github.com/godotengine/godot-cpp.git thirdparty/godot-cpp
   ```

2. Build the GDExtension:
   ```bash
   scons target=template_release platform=<platform>
   ```
   
   Where `<platform>` is one of: `linux`, `macos`, `windows`, `ios`, or `android`.
   
   Example:
   ```bash
   scons target=template_release platform=linux
   scons target=template_release platform=macos
   scons target=template_release platform=windows
   ```

The build system will automatically detect and use the system-installed erl_interface libraries. The GDExtension will be built in `bin/addons/godot_cnode/bin/`.

## Usage

1. Copy the `bin/addons/godot_cnode` directory to your Godot project's `addons/` folder
2. Enable the GDExtension in your Godot project
3. The CNode server will automatically start when Godot loads the GDExtension
4. Connect from Erlang/Elixir:
   ```elixir
   Node.connect(:"godot@127.0.0.1")
   ```

## Architecture

The CNode runs in a background thread and communicates with the current Godot instance using Erlang's native distribution protocol via `erl_interface`. It provides a GenServer-like API (implemented in pure C/C++) that exposes the entire Godot API dynamically. The implementation:

- **GenServer-style calls**: Synchronous operations via `{call, Module, Function, Args}`
- **GenServer-style casts**: Asynchronous operations via `{cast, Module, Function, Args}`
- **Dynamic API discovery**: Uses ClassDB to discover and call any Godot class, method, or property
- **Object management**: Track and operate on any Godot Object by instance ID
- **Singleton access**: Access any Godot singleton dynamically
- **Type conversion**: Automatic conversion between Godot Variants and Erlang BERT format

The API discovery system is inspired by the godot-sandbox project's runtime API generation, but adapted for Erlang/Elixir communication over CNode instead of sandboxed programs. The GenServer API pattern is inspired by bug-free-octo-parakeet but implemented entirely in C/C++ within the CNode.

## API

The CNode **only** exposes GenServer-style APIs. All access to Godot must go through the GenServer pattern: `{call, Module, Function, Args}` for synchronous operations and `{cast, Module, Function, Args}` for asynchronous operations.

### GenServer-style API (Pure C/C++)

The CNode implements a GenServer-like API pattern entirely in C/C++. This allows calling any Godot API dynamically:

**Synchronous Calls** (`{call, Module, Function, Args}`):
- `{call, godot, call_method, [ObjectID, MethodName, Args]}` - Call any method on any object
- `{call, godot, get_property, [ObjectID, PropertyName]}` - Get any property from any object
- `{call, godot, set_property, [ObjectID, PropertyName, Value]}` - Set any property on any object
- `{call, godot, get_singleton, [SingletonName]}` - Get any singleton by name
- `{call, godot, create_object, [ClassName]}` - Create an instance of any class
- `{call, godot, list_classes, []}` - List all registered Godot classes
- `{call, godot, get_class_methods, [ClassName]}` - Get methods for a class
- `{call, godot, get_class_properties, [ClassName]}` - Get properties for a class
- `{call, godot, get_singletons, []}` - List all singleton names
- `{call, godot, get_scene_tree_root, []}` - Get the root node of the scene tree
- `{call, godot, find_node, [NodePath]}` - Find a node by path string

**Asynchronous Casts** (`{cast, Module, Function, Args}`):
- `{cast, godot, call_method, [ObjectID, MethodName, Args]}` - Call method asynchronously
- `{cast, godot, set_property, [ObjectID, PropertyName, Value]}` - Set property asynchronously

**Example Usage from Elixir**:
```elixir
# Connect to CNode
Node.connect(:"godot@127.0.0.1")

# Synchronous call via GenServer pattern
GenServer.call(pid, {:call, :godot, :get_singleton, ["SceneTree"]})

# Asynchronous cast
GenServer.cast(pid, {:cast, :godot, :set_property, [object_id, "position", Vector3.new(1, 2, 3)]})

# Call any method on any object
GenServer.call(pid, {:call, :godot, :call_method, [node_id, "get_position", []]})

# Discover API dynamically
GenServer.call(pid, {:call, :godot, :list_classes, []})
GenServer.call(pid, {:call, :godot, :get_class_methods, ["Node"]})
```

## License

Same as Godot Engine (MIT)
