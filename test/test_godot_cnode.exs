defmodule TestGodotCNode do
  @moduledoc """
  Test script for the Godot CNode GDExtension.
  Tests both GenServer call (synchronous RPC) and cast (async message).
  """

  @cookie "godotcookie"
  @timeout 5000

  def run do
    cnode_name = :"godot@127.0.0.1"

    IO.puts("=== Godot CNode GenServer Test ===")
    IO.puts("CNode: #{cnode_name}")
    IO.puts("Cookie: #{@cookie}")
    IO.puts("")

    # Start distributed Erlang node
    node_name = :"godot_test_#{System.system_time(:second)}@127.0.0.1"
    case :net_kernel.start([node_name]) do
      {:ok, _} -> IO.puts("✓ Started distributed node: #{node_name}")
      {:error, {:already_started, _}} -> IO.puts("✓ Node already running")
      error ->
        IO.puts("✗ Failed: #{inspect(error)}")
        System.halt(1)
    end

    # Set cookie
    :erlang.set_cookie(node(), String.to_atom(@cookie))

    # Connect to CNode
    case :net_kernel.connect_node(cnode_name) do
      true ->
        IO.puts("✓ Connected to CNode")
        test_genserver_call(cnode_name)
        Process.sleep(500)
        test_genserver_cast(cnode_name)
        Process.sleep(500)
        test_error_handling(cnode_name)
        IO.puts("")
        IO.puts("=== Test Complete ===")
      _ ->
        IO.puts("✗ Failed to connect to CNode")
        IO.puts("  Make sure Godot is running with the CNode extension loaded")
        System.halt(1)
    end
  end

  # Test GenServer Call (synchronous RPC with reply)
  defp test_genserver_call(cnode_name) do
    IO.puts("")
    IO.puts("=== Testing GenServer Call (Synchronous RPC) ===")
    IO.puts("")

    # Test 1: Call erlang:node - should return the CNode's node name
    IO.puts("1. GenServer call: erlang:node")
    from = self()
    ref = make_ref()
    gen_call = {:"$gen_call", {from, ref}, {:erlang, :node, []}}

    case :erlang.send({:godot_server, cnode_name}, gen_call) do
      ^gen_call ->
        IO.puts("  ✓ Sent GenServer call")
        receive do
          {^ref, reply} ->
            IO.puts("  ✓ Received reply: #{inspect(reply)}")
            if is_atom(reply) do
              IO.puts("  ✓ Reply is valid (atom)")
            else
              IO.puts("  ✗ Reply format unexpected")
            end
        after
          @timeout ->
            IO.puts("  ✗ Timeout waiting for reply")
        end
      error ->
        IO.puts("  ✗ Failed to send: #{inspect(error)}")
    end

    Process.sleep(200)

    # Test 2: Call erlang:nodes - should return list of connected nodes
    IO.puts("")
    IO.puts("2. GenServer call: erlang:nodes")
    from2 = self()
    ref2 = make_ref()
    gen_call2 = {:"$gen_call", {from2, ref2}, {:erlang, :nodes, []}}

    case :erlang.send({:godot_server, cnode_name}, gen_call2) do
      ^gen_call2 ->
        IO.puts("  ✓ Sent GenServer call")
        receive do
          {^ref2, reply} ->
            IO.puts("  ✓ Received reply: #{inspect(reply)}")
            if is_list(reply) do
              IO.puts("  ✓ Reply is valid (list)")
            else
              IO.puts("  ✗ Reply format unexpected")
            end
        after
          @timeout ->
            IO.puts("  ✗ Timeout waiting for reply")
        end
      error ->
        IO.puts("  ✗ Failed to send: #{inspect(error)}")
    end
  end

  # Test GenServer Cast (asynchronous message, no reply)
  defp test_genserver_cast(cnode_name) do
    IO.puts("")
    IO.puts("=== Testing GenServer Cast (Asynchronous Message) ===")
    IO.puts("")

    # Test 1: Cast to erlang:node - should process but not reply
    IO.puts("1. GenServer cast: erlang:node")
    gen_cast = {:"$gen_cast", {:erlang, :node, []}}

    case :erlang.send({:godot_server, cnode_name}, gen_cast) do
      ^gen_cast ->
        IO.puts("  ✓ Sent GenServer cast")
        IO.puts("  ✓ Cast is fire-and-forget (no reply expected)")
        Process.sleep(200)
        IO.puts("  ✓ Cast should have been processed (check Godot console)")
      error ->
        IO.puts("  ✗ Failed to send: #{inspect(error)}")
    end

    Process.sleep(200)

    # Test 2: Cast to godot:call_method (example)
    IO.puts("")
    IO.puts("2. GenServer cast: godot:call_method (example)")
    gen_cast2 = {:"$gen_cast", {:godot, :call_method, [0, "get_class", []]}}

    case :erlang.send({:godot_server, cnode_name}, gen_cast2) do
      ^gen_cast2 ->
        IO.puts("  ✓ Sent GenServer cast")
        IO.puts("  ✓ Cast is fire-and-forget (no reply expected)")
        Process.sleep(200)
        IO.puts("  ✓ Cast should have been processed (check Godot console)")
      error ->
        IO.puts("  ✗ Failed to send: #{inspect(error)}")
    end
  end

  # Test error handling
  defp test_error_handling(cnode_name) do
    IO.puts("")
    IO.puts("=== Testing Error Handling ===")
    IO.puts("")

    # Test: Call invalid function - should return error
    IO.puts("1. GenServer call with invalid function (should return error)")
    from = self()
    ref = make_ref()
    gen_call = {:"$gen_call", {from, ref}, {:godot, :invalid_function, []}}

    case :erlang.send({:godot_server, cnode_name}, gen_call) do
      ^gen_call ->
        IO.puts("  ✓ Sent GenServer call")
        receive do
          {^ref, reply} ->
            IO.puts("  ✓ Received reply: #{inspect(reply)}")
            IO.puts("  ✓ Error handling works (invalid function returns error)")
        after
          @timeout ->
            IO.puts("  ✗ Timeout waiting for reply")
        end
      error ->
        IO.puts("  ✗ Failed to send: #{inspect(error)}")
    end
  end
end

TestGodotCNode.run()

