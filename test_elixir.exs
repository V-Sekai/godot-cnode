defmodule GodotCNodeTest do
  @cookie "godotcookie"
  @timeout 5000

  def run do
    hostname = case System.cmd("hostname", []) do
      {h, 0} -> String.trim(h)
      _ -> "127.0.0.1"
    end

    cnode_name = String.to_atom("godot@#{hostname}")

    IO.puts("=== Godot CNode Elixir Test ===")
    IO.puts("CNode: #{cnode_name}")
    IO.puts("Cookie: #{@cookie}")
    IO.puts("")

    # Start distributed Erlang node FIRST
    case :net_kernel.start([:"test@127.0.0.1"]) do
      {:ok, _pid} ->
        IO.puts("✓ Started distributed Erlang node")
      {:error, {:already_started, _pid}} ->
        IO.puts("✓ Distributed Erlang already started")
      error ->
        IO.puts("✗ Failed to start distributed Erlang: #{inspect(error)}")
        IO.puts("  Trying to start epmd...")
        start_epmd()
    end

    # Set cookie for Erlang distribution AFTER starting the node
    :erlang.set_cookie(node(), String.to_atom(@cookie))

    test_connection(cnode_name)
  end

  defp test_connection(cnode_name) do
    IO.puts("\nChecking epmd status...")
    case System.cmd("epmd", ["-names"]) do
      {output, 0} ->
        IO.puts("✓ epmd is running")
        if String.contains?(output, "godot") do
          IO.puts("  ✓ CNode is registered with epmd")
        else
          IO.puts("  ⚠ CNode not found in epmd (might be using ei_listen fallback)")
        end
      _ ->
        IO.puts("⚠ epmd is not running")
        IO.puts("  Starting epmd...")
        start_epmd()
    end

    IO.puts("\nConnecting to CNode: #{cnode_name}")
    IO.puts("  Attempting connection (this may take a moment)...")

    max_attempts = 5
    if try_connect_attempt(cnode_name, 1, max_attempts) do
      run_tests(cnode_name)
    else
      IO.puts("✗ Failed to connect to CNode after #{max_attempts} attempts")
      IO.puts("\nTroubleshooting:")
      IO.puts("  1. Make sure Godot is running with the CNode GDExtension loaded")
      IO.puts("  2. Check the CNode name in Godot output (should match: #{cnode_name})")
      IO.puts("  3. Check if CNode is accepting connections (look for 'ei_accept' messages in Godot console)")
      IO.puts("  4. Verify epmd can see the node: epmd -names")
      IO.puts("  5. Check cookie matches (expected: #{@cookie})")
      System.halt(1)
    end
  end

  defp try_connect_attempt(_cnode_name, attempt, max_attempts) when attempt > max_attempts do
    false
  end

  defp try_connect_attempt(cnode_name, attempt, max_attempts) do
    case :net_kernel.connect_node(cnode_name) do
      true ->
        IO.puts("✓ Connected to CNode! (attempt #{attempt})")
        true
      false ->
        if attempt < max_attempts do
          IO.puts("  Attempt #{attempt} failed, retrying in 1 second...")
          Process.sleep(1000)
          try_connect_attempt(cnode_name, attempt + 1, max_attempts)
        else
          false
        end
      :ignored ->
        IO.puts("✗ Connection to CNode ignored (attempt #{attempt})")
        IO.puts("  The CNode might not be registered with epmd or node name is invalid.")
        false
    end
  end

  defp run_tests(cnode_name) do
    IO.puts("\n=== Running Tests ===")

    # Test 1: Check node is in connected nodes
    IO.puts("\n1. Checking connected nodes...")
    nodes = :erlang.nodes()
    if cnode_name in nodes do
      IO.puts("  ✓ CNode is in connected nodes list")
    else
      IO.puts("  ✗ CNode not in connected nodes list")
      IO.puts("    Connected nodes: #{inspect(nodes)}")
    end

    # Test 2: RPC call to get node name
    IO.puts("\n2. Testing RPC call...")
    case :rpc.call(cnode_name, :erlang, :node, [], @timeout) do
      {:badrpc, reason} ->
        IO.puts("  ✗ RPC call failed: #{inspect(reason)}")
      result ->
        IO.puts("  ✓ RPC call succeeded: #{inspect(result)}")
    end

    # Test 3: Send a message
    IO.puts("\n3. Testing message send...")
    message = {:test, :hello, "from Elixir"}
    try do
      send({:godot_server, cnode_name}, message)
      IO.puts("  ✓ Message sent successfully")
    rescue
      e ->
        IO.puts("  ✗ Failed to send message: #{inspect(e)}")
    end

    IO.puts("\n=== Tests Complete ===")
  end

  defp start_epmd do
    case System.cmd("epmd", ["-daemon"]) do
      {_, 0} ->
        IO.puts("  ✓ epmd started")
        Process.sleep(500)  # Give epmd time to start
      _ ->
        IO.puts("  ⚠ Failed to start epmd (might already be running)")
    end
  end
end

GodotCNodeTest.run()
