# Agents Architecture

This document describes the agent architecture and communication patterns for the Godot CNode GDExtension.

## Overview

The Godot CNode acts as an **agent** that bridges Erlang/Elixir nodes with the Godot Engine. It runs as a background server thread within Godot and communicates using the Erlang distribution protocol.

## Agent Components

### 1. CNode Server Agent

The CNode server is the main agent that:

- **Runs in a background thread** - Started automatically when the GDExtension loads
- **Listens for connections** - Accepts connections from Erlang/Elixir nodes via `ei_accept()`
- **Processes messages** - Handles both synchronous (GenServer calls) and asynchronous (GenServer casts, plain messages) operations
- **Manages connections** - Maintains connections to remote Erlang/Elixir nodes
- **Registers global names** - Registers as `godot_server` for easy message routing

**Location**: `src/godot_cnode.cpp` - `main_loop()` function

### 2. Message Processing Agent

The message processor handles different message formats:

- **GenServer-style messages** (`$gen_call`, `$gen_cast`) - For synchronous and asynchronous operations
- **Plain messages** - Simple tuples `{Module, Function, Args}` for fire-and-forget operations
- **BERT encoding/decoding** - Converts between Erlang terms and Godot Variants

**Location**: `src/godot_cnode.cpp` - `process_message()`, `handle_call()`, `handle_cast()`

### 3. Socket Callback Agent (macOS Compatibility)

Custom socket callbacks provide macOS compatibility:

- **Custom `accept` callback** - Bypasses `SO_ACCEPTCONN` checks that fail on macOS
- **Delegates other operations** - Most socket operations use default `erl_interface` implementations
- **Blocking mode enforcement** - Ensures sockets are in blocking mode for reliable operation

**Location**: `src/godot_cnode.cpp` - `macos_tcp_accept()`, `custom_socket_callbacks`

## Communication Patterns

### Synchronous RPC (GenServer Call)

**Pattern**: `{'$gen_call', {From, Tag}, {Module, Function, Args}}`

**Flow**:
1. Elixir client sends GenServer call message
2. CNode receives message via `ei_receive_msg()`
3. `process_message()` routes to `handle_call()`
4. `handle_call()` processes the request and builds a reply
5. `send_reply()` encodes reply as `{Tag, Reply}` and sends via `ei_send()`
6. Elixir client receives reply in `receive` block

**Example**:
```elixir
# Elixir client
from = self()
ref = make_ref()
gen_call_message = {'$gen_call', {from, ref}, {:erlang, :node, []}}
:erlang.send({:godot_server, :"godot@127.0.0.1"}, gen_call_message)

receive do
  {^ref, reply} -> IO.puts("Reply: #{inspect(reply)}")
after
  5000 -> IO.puts("Timeout")
end
```

**CNode Processing**:
```cpp
// In handle_call()
if (strcmp(module, "erlang") == 0 && strcmp(function, "node") == 0) {
    ei_x_encode_tuple_header(&reply, 2);
    ei_x_encode_atom(&reply, "reply");
    ei_x_encode_atom(&reply, ec.thisnodename); // Return node name
    send_reply(&reply, fd, from_pid, tag_ref);
}
```

### Asynchronous Cast (GenServer Cast)

**Pattern**: `{'$gen_cast', {Module, Function, Args}}`

**Flow**:
1. Elixir client sends GenServer cast message
2. CNode receives message via `ei_receive_msg()`
3. `process_message()` routes to `handle_cast()`
4. `handle_cast()` processes the request (no reply sent)
5. Console output shows processing status

**Example**:
```elixir
# Elixir client
gen_cast_message = {'$gen_cast', {:godot, :call_method, [12345, "get_name", []]}}
:erlang.send({:godot_server, :"godot@127.0.0.1"}, gen_cast_message)
# No reply expected - fire and forget
```

**CNode Processing**:
```cpp
// In handle_cast()
printf("Godot CNode: Processing async message - Module: %s, Function: %s\n", module, function);
// Process the request...
printf("Godot CNode: Async message processing complete\n");
```

### Plain Message (Fire-and-Forget)

**Pattern**: `{Module, Function, Args}`

**Flow**:
1. Elixir client sends plain tuple message
2. CNode receives message via `ei_receive_msg()`
3. `process_message()` detects plain message format
4. Routes to `handle_cast()` (same as GenServer cast)
5. Console output shows processing status

**Example**:
```elixir
# Elixir client
message = {:erlang, :node, []}
:erlang.send({:godot_server, :"godot@127.0.0.1"}, message)
# No reply expected
```

**CNode Processing**:
```cpp
// In process_message()
if (strcmp(atom, "$gen_call") != 0 && strcmp(atom, "$gen_cast") != 0) {
    // Plain message - reset index and handle as cast
    *index = saved_index;
    printf("Godot CNode: Received plain message (asynchronous, no reply)\n");
    return handle_cast(buf, index);
}
```

## Message Routing

### Module-Based Routing

The CNode routes messages based on the module name:

**`erlang` module**:
- `erlang:node` - Returns the CNode's node name
- `erlang:nodes` - Returns list of connected nodes (currently empty)

**`godot` module**:
- `godot:call_method` - Call any method on any Godot object
- `godot:set_property` - Set any property on any Godot object
- `godot:get_property` - Get any property from any Godot object
- `godot:get_scene_tree` - Get SceneTree information
- (More functions can be added)

### Function Implementation

**Synchronous (handle_call)**:
- Processes request
- Builds reply buffer
- Sends reply via `send_reply()`
- Returns success/error status

**Asynchronous (handle_cast)**:
- Processes request
- Logs to console
- No reply sent
- Returns success/error status

## Platform-Specific Handling

### macOS Compatibility

**Issue**: `SO_ACCEPTCONN` socket option is not supported on macOS, causing `ei_accept()` and `ei_receive_msg()` to fail with `errno 42` (Protocol not available).

**Solution**: Custom socket callbacks that bypass the problematic checks:

1. **Custom `accept` callback** - Directly calls `accept()` after ensuring blocking mode
2. **Error handling** - When `ei_receive_msg()` returns `errno 42`, attempt to process message from buffer or read directly from socket
3. **Buffer fallback** - If buffer has data, process it despite the error
4. **Raw socket read** - If buffer is empty, attempt raw `read()` from socket and decode manually

**Implementation**:
```cpp
// Custom accept callback
static int macos_tcp_accept(void **ctx, void *addr, int *len, unsigned unused) {
    // Extract FD, ensure blocking mode, call accept() directly
    // Bypasses SO_ACCEPTCONN check
}

// Error handling in main_loop
if (saved_errno == 42 || saved_errno == ENOPROTOOPT) {
    if (x.index > 0) {
        // Process from buffer
    } else {
        // Try raw socket read
        ssize_t bytes_read = read(fd, raw_buf, sizeof(raw_buf));
        // Decode and process...
    }
}
```

## Connection Lifecycle

1. **Initialization** (`init_cnode`):
   - Initialize `ei_cnode` structure
   - Create listening socket with `ei_listen()`
   - Publish with epmd (with error handling for macOS)
   - Register global name `godot_server`

2. **Main Loop** (`main_loop`):
   - Wait for connections with `select()`
   - Accept connection with `ei_accept()` (using custom callback on macOS)
   - Receive message with `ei_receive_msg()` (with errno 42 handling)
   - Process message with `process_message()`
   - Send reply if needed (for GenServer calls)
   - Close connection after processing

3. **Message Processing**:
   - Decode message format (GenServer vs plain)
   - Route to appropriate handler (`handle_call` or `handle_cast`)
   - Convert BERT to Godot Variants
   - Execute Godot API calls
   - Convert results back to BERT
   - Send reply (for synchronous calls)

## Threading Model

- **Main Thread**: Godot engine runs on main thread
- **CNode Thread**: Background thread for Erlang distribution protocol
- **Thread Safety**: Godot API calls from CNode thread must use `call_deferred()` or `call_thread_group()` (currently returns error to prevent crashes)

## Error Handling

### Connection Errors

- **`ei_accept()` failures**: Retry with exponential backoff
- **`ei_receive_msg()` errno 42**: Attempt buffer processing or raw socket read
- **Connection aborted**: Retry immediately
- **Bad file descriptor**: Exit main loop

### Message Processing Errors

- **Invalid message format**: Log error, return -1
- **Decoding errors**: Log error, return -1
- **Godot API errors**: Return error in reply (for calls) or log (for casts)

## Testing

### Test Scripts

- **`test/cnode_test_plain_rpc.exs`**: Tests plain message format (async)
- **`test/cnode_test_genserver_rpc.exs`**: Tests GenServer call format (sync with replies)
- **`test/cnode_test_connect.exs`**: Tests basic connectivity

### Test Patterns

**Plain RPC Test**:
```elixir
message = {:erlang, :node, []}
:erlang.send({:godot_server, cnode_name}, message)
# Check console output for processing status
```

**GenServer RPC Test**:
```elixir
from = self()
ref = make_ref()
gen_call = {'$gen_call', {from, ref}, {:erlang, :node, []}}
:erlang.send({:godot_server, cnode_name}, gen_call)
receive do
  {^ref, reply} -> # Process reply
after
  5000 -> # Timeout
end
```

## Console Output

The CNode provides detailed console output for debugging:

- **Connection events**: "✓ Accepted connection", "Connected from node"
- **Message reception**: "Received plain message", "Received GenServer call"
- **Processing status**: "Processing async message - Module: X, Function: Y"
- **Completion**: "Async message processing complete"
- **Errors**: Error messages with errno and description

All output is flushed immediately with `fflush(stdout)` for real-time visibility.

## Future Enhancements

- **Thread-safe Godot API calls**: Implement `call_deferred()` for safe API access from background thread
- **More Godot functions**: Expand `godot` module with more API functions
- **Connection pooling**: Maintain multiple connections instead of one-at-a-time
- **Message queuing**: Queue messages when Godot is busy
- **Health checks**: Periodic health check messages
- **Metrics**: Track message counts, latency, errors
