# CNode Test Project

This is a test project for the Godot CNode GDExtension.

## Building

1. Build the GDExtension from the project root:
   ```bash
   scons target=template_release platform=macos arch=arm64
   ```

   The GDExtension will be built to `bin/addons/godot_cnode/bin/`

2. This test project references the built library using relative paths.

## Usage

1. Open this project in Godot:
   ```bash
   godot --path test_project
   ```

2. The CNode server will automatically start when the project loads.

3. Connect from Erlang/Elixir:
   ```elixir
   Node.connect(:"godot@127.0.0.1")
   ```

## Cookie Location

The CNode cookie is stored in:
- macOS: `~/Library/Application Support/Godot/app_userdata/CNode Test Project/cnode_cookie`
- Linux: `~/.local/share/godot/app_userdata/CNode Test Project/cnode_cookie`
- Windows: `%APPDATA%/Godot/app_userdata/CNode Test Project/cnode_cookie`

The cookie is automatically generated on first run if it doesn't exist.

