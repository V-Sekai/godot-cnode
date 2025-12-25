defmodule GodotCNodePlainRPCTest do
  @cookie "godotcookie"

  def run do
    # Try both localhost and hostname
    cnode_name = :"godot@127.0.0.1"

    IO.puts("=== Godot CNode Plain RPC Test ===")
    IO.puts("CNode: #{cnode_name}")
    IO.puts("Cookie: #{@cookie}")
    IO.puts("")
    IO.puts("Testing plain RPC calls: {Module, Function, Args}")
    IO.puts("")

    # Start distributed Erlang node
    node_name = :"plain_rpc_test_#{System.system_time(:second)}@127.0.0.1"
    case :net_kernel.start([node_name]) do
      {:ok, _} -> IO.puts("✓ Started distributed node: #{node_name}")
      {:error, {:already_started, _}} -> IO.puts("✓ Node already running")
      error ->
        IO.puts("✗ Failed: #{inspect(error)}")
        System.halt(1)
    end

    # Set cookie
    :erlang.set_cookie(node(), String.to_atom(@cookie))

    # Connect
    case :net_kernel.connect_node(cnode_name) do
      true ->
        IO.puts("✓ Connected to CNode")
        test_plain_rpc(cnode_name)
      _ ->
        IO.puts("✗ Failed to connect")
        System.halt(1)
    end
  end

  defp test_plain_rpc(cnode_name) do
    IO.puts("")
    IO.puts("=== Testing Plain RPC Calls ===")
    IO.puts("")

    # Test 1: Plain RPC call to erlang:node
    IO.puts("1. Testing plain RPC: {erlang, node, []}")
    message1 = {:erlang, :node, []}
    result1 = :erlang.send({:godot_server, cnode_name}, message1)
    if result1 == message1 do
      IO.puts("  ✓ Sent plain RPC call")
      Process.sleep(500) # Give CNode time to process
    else
      IO.puts("  ✗ Failed to send: #{inspect(result1)}")
    end

    # Test 2: Plain RPC call to godot:get_scene_tree
    IO.puts("")
    IO.puts("2. Testing plain RPC: {godot, get_scene_tree, []}")
    message2 = {:godot, :get_scene_tree, []}
    result2 = :erlang.send({:godot_server, cnode_name}, message2)
    if result2 == message2 do
      IO.puts("  ✓ Sent plain RPC call")
      Process.sleep(500) # Give CNode time to process
    else
      IO.puts("  ✗ Failed to send: #{inspect(result2)}")
    end

    # Test 3: Plain RPC call with arguments
    IO.puts("")
    IO.puts("3. Testing plain RPC: {godot, call_method, [12345, \"get_name\", []]}")
    message3 = {:godot, :call_method, [12345, "get_name", []]}
    result3 = :erlang.send({:godot_server, cnode_name}, message3)
    if result3 == message3 do
      IO.puts("  ✓ Sent plain RPC call")
      Process.sleep(500) # Give CNode time to process
    else
      IO.puts("  ✗ Failed to send: #{inspect(result3)}")
    end

    IO.puts("")
    IO.puts("=== Test Complete ===")
    IO.puts("")
    IO.puts("Note: Plain RPC calls are fire-and-forget (no reply expected)")
    IO.puts("      For synchronous calls with replies, use GenServer format")
  end
end

GodotCNodePlainRPCTest.run()
