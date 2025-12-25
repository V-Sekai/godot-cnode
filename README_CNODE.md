# Godot CNode

Erlang/Elixir CNode interface for Godot Engine.

This project provides a standalone executable that allows Erlang/Elixir nodes to communicate with Godot using the Erlang distribution protocol.

## Building

The CNode is built as a standalone executable that links against Godot's static library.

```bash
scons target=template_release
```

## Usage

Run the CNode executable:

```bash
./bin/godot_cnode -name godot@127.0.0.1 -setcookie godotcookie
```

Then connect from Erlang/Elixir:

```elixir
Node.connect(:"godot@127.0.0.1")
```

