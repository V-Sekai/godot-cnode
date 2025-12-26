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
- **Uses `ei_connect_xinit_ussi`** - Initializes CNode with custom socket callbacks for macOS support

**Location**: `src/godot_cnode.cpp` - `macos_tcp_accept()`, `custom_socket_callbacks`

**Implementation**: The custom socket callbacks structure delegates most operations to `ei_default_socket_callbacks`, but overrides the `accept` callback with `macos_tcp_accept()` which directly calls `accept()` after ensuring the socket is in blocking mode.

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

### RPC Message Format (Rex)

**Pattern**: `{rex, From, {'$gen_call', {From, Tag}, Request}}`

**Flow**:
1. Elixir client sends message to registered name using `:erlang.send()`
2. CNode receives message in `rex` format (RPC message format)
3. `process_message()` detects `rex` atom and extracts the GenServer call
4. Routes to `handle_call()` with extracted From PID and Tag
5. Reply is sent back to the client

**Note**: When sending to a registered global name (e.g., `:godot_server`), Erlang wraps the message in a `rex` tuple. The CNode handles this format by extracting the inner GenServer call message.

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

**Issue**: `SO_ACCEPTCONN` socket option is not supported on macOS, causing `ei_accept()` to fail with `errno 42` (Protocol not available) when using default socket callbacks.

**Solution**: Custom socket callbacks that bypass the problematic checks:

1. **Use `ei_connect_xinit_ussi`** - Initialize CNode with custom socket callbacks instead of `ei_connect_init`
2. **Custom `accept` callback** - Directly calls `accept()` after ensuring blocking mode, bypasses `SO_ACCEPTCONN` check
3. **Delegate other operations** - Most socket operations (socket, close, listen, connect, read, write, etc.) delegate to `ei_default_socket_callbacks`
4. **Blocking mode enforcement** - Ensures sockets are in blocking mode for reliable operation

**Implementation**:
```cpp
// Custom accept callback
static int macos_tcp_accept(void **ctx, void *addr, int *len, unsigned unused) {
    int fd, res;
    socklen_t addr_len = (socklen_t)*len;

    // Extract file descriptor from context
    res = EI_DFLT_CTX_TO_FD__(*ctx, &fd);
    if (res) return res;

    // Ensure socket is in blocking mode
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
    }

    // Call accept() directly - bypasses SO_ACCEPTCONN check
    res = accept(fd, (struct sockaddr *)addr, &addr_len);
    if (res < 0) return errno;

    *len = (int)addr_len;
    *ctx = EI_FD_AS_CTX__(res);
    return 0;
}

// Initialize with custom callbacks
res = ei_connect_xinit_ussi(&ec, thishostname, thisalivename, thisnodename,
        NULL, cookie, 0,
        &custom_socket_callbacks,
        sizeof(custom_socket_callbacks),
        NULL);
```

**Status**: ✅ Fully implemented and working. Both the Godot CNode and standalone test CNode use this approach for macOS compatibility.

## Connection Lifecycle

1. **Initialization** (`init_cnode`):
   - Zero-initialize `ei_cnode` structure
   - Call `ei_init()` to initialize erl_interface library
   - Initialize CNode with `ei_connect_xinit_ussi()` using custom socket callbacks (macOS compatibility)
   - Create listening socket with `ei_listen()` to get an ephemeral port
   - Publish with epmd using `ei_publish()` with the specific port from `ei_listen()`
   - Keep `publish_fd` open to maintain epmd registration
   - Store `listen_fd` for accepting connections

2. **Main Loop** (`main_loop`):
   - Wait for connections with `select()` on `listen_fd` (optional, for non-blocking behavior)
   - Accept connection with `ei_accept()` (uses custom callback on macOS)
   - Register global name (e.g., `godot_server` or `test_server`) using `ei_global_register()`
   - Process messages in a loop:
     - Use `select()` to wait for data before calling `ei_receive_msg()` (prevents blocking on empty connections)
     - Receive message with `ei_receive_msg()`
     - Process message with `process_message()`
     - Send reply if needed (for GenServer calls)
   - Close connection after processing or on error

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

- **`ei_accept()` failures**: Handle specific error codes:
  - `EBADF` (9): Socket closed, exit main loop
  - `ECONNABORTED` (53): Connection aborted, retry
  - `EINTR`: Interrupted by signal, retry
  - Other errors: Log and retry after short delay
- **`ei_receive_msg()` errors**:
  - `ERL_TICK`: Keepalive message, continue
  - `ERL_ERROR` with `ECONNRESET`/`EPIPE`: Connection closed, break loop
  - `ERL_ERROR` with errno 0: May be false positive, check socket state before closing
  - Use `select()` before `ei_receive_msg()` to avoid blocking on empty connections
- **Connection aborted**: Retry immediately
- **Bad file descriptor**: Exit main loop

### Message Processing Errors

- **Invalid message format**: Log error, return -1
- **Decoding errors**: Log error, return -1
- **Godot API errors**: Return error in reply (for calls) or log (for casts)

## Testing

### Test Scripts

- **`test/cnode_test_genserver_simple.exs`**: Simple GenServer test
- **`test/cnode_test_genserver_rpc.exs`**: Full GenServer RPC test with multiple handlers
- **`test/test_cnode_elixir.exs`**: Test script for standalone CNode

### Standalone CNode Test Implementation

A standalone C-based CNode (`test/test_cnode.c`) has been created for debugging and testing GenServer functionality:

- **Purpose**: Test GenServer message handling without Godot dependencies
- **Features**: Full GenServer call/cast support, global name registration, macOS socket callbacks
- **Build**: `cd test && make`
- **Run**: `./test_cnode test_cnode@127.0.0.1 godotcookie`
- **Test**: `elixir test/test_cnode_elixir.exs`

**Status**: ✅ Accepts connections, registers with epmd, processes GenServer casts. ⚠️ GenServer calls (sync RPC) need further debugging.

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
gen_call = {:"$gen_call", {from, ref}, {:erlang, :node, []}}
:erlang.send({:godot_server, cnode_name}, gen_call)
receive do
  {^ref, reply} -> # Process reply
after
  5000 -> # Timeout
end
```

**GenServer Cast Test**:
```elixir
gen_cast = {:"$gen_cast", {:erlang, :node, []}}
:erlang.send({:godot_server, cnode_name}, gen_cast)
# No reply expected - fire and forget
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
- **Standalone CNode**: Complete GenServer call (sync RPC) support in standalone test CNode
- **Connection persistence**: Keep connections open for multiple messages instead of closing after each message
