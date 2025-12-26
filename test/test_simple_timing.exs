defmodule SimpleTimingTest do
  @cookie "godotcookie"
  @timeout 5000

  def run do
    # Find the CNode port from epmd
    case :erl_epmd.names() do
      {:ok, names} ->
        case List.keyfind(names, 'simple_test', 0) do
          {_, port} ->
            IO.puts("Found simple_test on port: #{port}")
            test_timing(port)
          nil ->
            IO.puts("simple_test not found in epmd")
            IO.puts("Available nodes: #{inspect(names)}")
        end
      error ->
        IO.puts("Failed to query epmd: #{inspect(error)}")
    end
  end

  defp test_timing(cnode_port) do
    # Start distributed node
    node_name = :"timing_test_#{System.system_time(:second)}@127.0.0.1"
    case :net_kernel.start([node_name]) do
      {:ok, _} -> IO.puts("✓ Started node: #{node_name}")
      {:error, {:already_started, _}} -> IO.puts("✓ Node already running")
      error ->
        IO.puts("✗ Failed: #{inspect(error)}")
        System.halt(1)
    end

    :erlang.set_cookie(node(), String.to_atom(@cookie))

    # Connect to CNode
    cnode_name = :"simple_test@127.0.0.1"
    case :net_kernel.connect_node(cnode_name) do
      true ->
        IO.puts("✓ Connected to CNode")
        test_messages(cnode_name)
      _ ->
        IO.puts("✗ Failed to connect")
        System.halt(1)
    end
  end

  defp test_messages(cnode_name) do
    IO.puts("\n=== Testing Message Timing ===\n")

    # Test 1: First message
    IO.puts("1. Sending first message (erlang:node)...")
    from1 = self()
    ref1 = make_ref()
    gen_call1 = {'$gen_call', {from1, ref1}, {:erlang, :node, []}}

    send_time1 = System.monotonic_time(:millisecond)
    case :erlang.send({:simple_test, cnode_name}, gen_call1) do
      ^gen_call1 ->
        IO.puts("  ✓ Sent at #{send_time1}ms")
        receive do
          {^ref1, reply} ->
            recv_time1 = System.monotonic_time(:millisecond)
            elapsed1 = recv_time1 - send_time1
            IO.puts("  ✓ Received reply: #{inspect(reply)} (elapsed: #{elapsed1}ms)")
        after
          @timeout ->
            timeout_time1 = System.monotonic_time(:millisecond)
            elapsed1 = timeout_time1 - send_time1
            IO.puts("  ✗ Timeout after #{elapsed1}ms")
        end
      error ->
        IO.puts("  ✗ Failed to send: #{inspect(error)}")
    end

    Process.sleep(500)

    # Test 2: Second message
    IO.puts("\n2. Sending second message (invalid_function)...")
    from2 = self()
    ref2 = make_ref()
    gen_call2 = {'$gen_call', {from2, ref2}, {:godot, :invalid_function, []}}

    send_time2 = System.monotonic_time(:millisecond)
    case :erlang.send({:simple_test, cnode_name}, gen_call2) do
      ^gen_call2 ->
        IO.puts("  ✓ Sent at #{send_time2}ms")
        receive do
          {^ref2, reply} ->
            recv_time2 = System.monotonic_time(:millisecond)
            elapsed2 = recv_time2 - send_time2
            IO.puts("  ✓ Received reply: #{inspect(reply)} (elapsed: #{elapsed2}ms)")
        after
          @timeout ->
            timeout_time2 = System.monotonic_time(:millisecond)
            elapsed2 = timeout_time2 - send_time2
            IO.puts("  ✗ Timeout after #{elapsed2}ms")
        end
      error ->
        IO.puts("  ✗ Failed to send: #{inspect(error)}")
    end

    IO.puts("\n=== Test Complete ===")
  end
end

SimpleTimingTest.run()
