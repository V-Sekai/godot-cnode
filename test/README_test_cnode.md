# Standalone C CNode with GenServer Features

This directory contains a standalone C implementation of a CNode with full GenServer functionality. It's used for debugging reply issues by comparing behavior with the Godot CNode.

## Overview

The standalone CNode implements:

- **GenServer Call** (`$gen_call`) - Synchronous RPC with replies
- **GenServer Cast** (`$gen_cast`) - Asynchronous messages, no reply
- **Global name registration** - Registers as `test_server` for easy access
- **Multiple handlers** - `erlang:node`, `erlang:nodes`, `test:ping`, `test:echo`

## Building

### Prerequisites

- Erlang/OTP installed (provides `erl_interface` library)
- `gcc` or compatible C compiler
- `make`

### Build Steps

1. Navigate to the test directory:

   ```bash
   cd test
   ```

2. Build the standalone CNode:

   ```bash
   make
   ```

   The Makefile will automatically detect your Erlang installation and find `erl_interface`.

3. If the build fails, you can manually specify the erl_interface path:
   ```bash
   make ERL_INTERFACE_BASE=/path/to/erl_interface-5.6.1
   ```

### Build Output

The build produces:

- `test_cnode` - The standalone CNode binary (or `test_cnode.exe` on Windows)

## Running

### Start epmd

The Erlang Port Mapper Daemon must be running:

```bash
epmd -daemon
```

### Run the CNode

```bash
./test_cnode test_cnode@127.0.0.1 godotcookie
```

Or use the Makefile target:

```bash
make run
```

The CNode will:

1. Initialize and register with epmd
2. Create a listening socket
3. Wait for connections
4. Process GenServer calls and casts
5. Register global name `test_server` on first connection

Press `Ctrl+C` to exit.

## Testing

### Run the Elixir Test

In a separate terminal:

```bash
elixir test/test_cnode_elixir.exs
```

The test will:

1. Connect to the standalone CNode
2. Test GenServer calls (should receive replies)
3. Test GenServer casts (fire-and-forget)
4. Test custom handlers (`test:ping`, `test:echo`)

### Expected Output

**CNode console:**

```
=== Standalone C CNode with GenServer Features ===
Nodename: test_cnode@127.0.0.1
Cookie: godotcookie

Test CNode: Created listening socket on port 54321
Test CNode: Successfully published node with epmd on port 54321
Test CNode: Ready for connections (fd: 3, port: 54321)
Test CNode: Entering main loop
Press Ctrl+C to exit

Test CNode: ✓ Accepted connection on fd: 4
Test CNode: Connected from node: test_client_1234567890@127.0.0.1
Test CNode: ✓ Registered global name 'test_server'
Test CNode: Received GenServer call (synchronous RPC with reply)
Test CNode: handle_call - Module=erlang, Function=node
Test CNode: Reply sent successfully (42 bytes)
```

**Elixir test output:**

```
=== Standalone C CNode GenServer Test ===
CNode: test_cnode@127.0.0.1
Cookie: godotcookie

✓ Started distributed node: test_client_1234567890@127.0.0.1
✓ Connected to CNode

=== Testing GenServer Call (Synchronous RPC) ===

1. GenServer call: erlang:node
  ✓ Sent GenServer call
  ✓ Received reply: :"test_cnode@127.0.0.1"
  ✓ Reply is valid (atom)

2. GenServer call: erlang:nodes
  ✓ Sent GenServer call
  ✓ Received reply: []
  ✓ Reply is valid (list)

=== Testing GenServer Cast (Asynchronous Message) ===
...
```

## Comparison with Godot CNode

### Key Differences

| Feature              | Standalone CNode                              | Godot CNode                           |
| -------------------- | --------------------------------------------- | ------------------------------------- |
| **Processing Model** | Blocking `ei_accept()` and `ei_receive_msg()` | Frame-based `process_cnode_frame()`   |
| **Threading**        | Single-threaded main loop                     | Background thread + Godot main thread |
| **Dependencies**     | Pure C + erl_interface                        | C++ + Godot API + erl_interface       |
| **Message Handling** | Direct socket I/O                             | Frame-based polling                   |
| **Reply Sending**    | Immediate via `ei_send()`                     | Immediate via `ei_send()`             |

### Debugging Strategy

1. **If standalone CNode works but Godot CNode doesn't:**

   - Issue is likely in Godot integration
   - Check frame-based processing (`process_cnode_frame`)
   - Check thread safety issues
   - Check Godot API calls interfering with socket I/O

2. **If standalone CNode also fails:**

   - Issue is likely in Erlang distribution protocol
   - Check `ei_send()` usage
   - Check reply format encoding
   - Check socket state

3. **If both work but replies timeout:**
   - Check message routing
   - Check registered name lookup
   - Check connection state

## Handlers

### Built-in Handlers

- **`erlang:node`** - Returns the CNode's nodename (atom)
- **`erlang:nodes`** - Returns list of connected nodes (empty list)

### Custom Handlers

- **`test:ping`** - Returns `:pong` atom
- **`test:echo`** - Echoes back the first argument (supports integers and strings)

## Troubleshooting

### Build Issues

**Error: Could not find erl_interface**

- Install Erlang/OTP: `brew install erlang` (macOS) or `apt-get install erlang-dev` (Linux)
- Or set `ERL_INTERFACE_BASE` manually in the Makefile

**Error: ei.h: No such file or directory**

- Check that `erl_interface` is installed
- Verify the include path in Makefile

### Runtime Issues

**Error: epmd is not running**

- Start epmd: `epmd -daemon`
- Check status: `epmd -names`

**Error: Connection refused**

- Make sure the CNode is running
- Check that epmd is running
- Verify nodename and cookie match

**Replies timeout**

- Check CNode console for error messages
- Verify `ei_send()` is being called
- Check reply buffer encoding
- Compare with working standalone CNode behavior

## Files

- `test_cnode.c` - Standalone CNode implementation
- `Makefile` - Build configuration
- `test_cnode_elixir.exs` - Elixir test script
- `README_test_cnode.md` - This file

## Cleanup

Remove build artifacts:

```bash
make clean
```

This removes:

- `test_cnode` binary
- `test_cnode.o` object file
