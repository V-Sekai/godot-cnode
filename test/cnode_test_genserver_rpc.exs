defmodule GodotCNodeGenServerRPCTest do
  @cookie "godotcookie"
  @timeout 5000

  def run do
    cnode_name = :"godot@127.0.0.1"

    IO.puts("=== Godot CNode GenServer RPC Test ===")
    IO.puts("CNode: #{cnode_name}")
    IO.puts("Cookie: #{@cookie}")
    IO.puts("")
    IO.puts("Testing GenServer-style synchronous RPC calls (with replies)")
    IO.puts("")

    # Start distributed Erlang node
    node_name = :"genserver_rpc_test_#{System.system_time(:second)}@127.0.0.1"
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
        test_genserver_rpc(cnode_name)
      _ ->
        IO.puts("✗ Failed to connect")
        System.halt(1)
    end
  end

  defp test_genserver_rpc(cnode_name) do
    IO.puts("")
    IO.puts("=== Testing GenServer Synchronous RPC Calls ===")
    IO.puts("")

    # Test 1: GenServer call to erlang:node
    IO.puts("1. Testing GenServer call: {:\"$gen_call\", {From, Tag}, {:erlang, :node, []}}")
    from = self()
    ref = make_ref()
    gen_call_message = {:"$gen_call", {from, ref}, {:erlang, :node, []}}

    case :erlang.send({:godot_server, cnode_name}, gen_call_message) do
      ^gen_call_message ->
        IO.puts("  ✓ Sent GenServer call message")
        receive do
          {^ref, reply} ->
            IO.puts("  ✓ Received reply: #{inspect(reply)}")
        after
          @timeout ->
            IO.puts("  ✗ No reply received (timeout)")
        end
      error ->
        IO.puts("  ✗ Failed to send: #{inspect(error)}")
    end

    Process.sleep(500)

    # Test 2: GenServer call to a non-existent function (should return error)
    IO.puts("")
    IO.puts("2. Testing GenServer call with invalid function (should return error)")
    from2 = self()
    ref2 = make_ref()
    gen_call_message2 = {:"$gen_call", {from2, ref2}, {:godot, :invalid_function, []}}

    case :erlang.send({:godot_server, cnode_name}, gen_call_message2) do
      ^gen_call_message2 ->
        IO.puts("  ✓ Sent GenServer call message")
        receive do
          {^ref2, reply} ->
            IO.puts("  ✓ Received reply: #{inspect(reply)}")
        after
          @timeout ->
            IO.puts("  ✗ No reply received (timeout)")
        end
      error ->
        IO.puts("  ✗ Failed to send: #{inspect(error)}")
    end

    IO.puts("")
    IO.puts("=== Test Complete ===")
  end
end

GodotCNodeGenServerRPCTest.run()
