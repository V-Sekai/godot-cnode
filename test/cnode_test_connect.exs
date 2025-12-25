defmodule GodotCNodeTest do
  @cookie "godotcookie"
  @timeout 5000

  def run do
    # Use localhost since it's more reliable than .local hostnames
    # The CNode code has fallback logic that tries localhost if hostname fails
    # Check epmd to see which node name is actually registered
    cnode_name = case System.cmd("epmd", ["-names"]) do
      {output, 0} ->
        # Prefer 127.0.0.1 since it's always valid (localhost may be rejected)
        cond do
          String.contains?(output, "godot@127.0.0.1") ->
            :"godot@127.0.0.1"
          String.contains?(output, "godot@localhost") ->
            :"godot@localhost"
          true ->
            # Extract the actual registered name from epmd output
            case Regex.run(~r/name (godot@[\w\.-]+)/, output) do
              [_, name] -> String.to_atom(name)
              _ -> :"godot@127.0.0.1"  # Default fallback to IP
            end
        end
      _ ->
        :"godot@127.0.0.1"  # Default if epmd check fails
    end

    IO.puts("=== Godot CNode Elixir Test ===")
    IO.puts("CNode: #{cnode_name}")
    IO.puts("Cookie: #{@cookie}")
    IO.puts("")

    # Start distributed Erlang node FIRST
    # Use a unique node name based on timestamp to avoid conflicts
    unique_name = :"test_#{System.system_time(:second)}@127.0.0.1"

    case :net_kernel.start([unique_name]) do
      {:ok, _pid} ->
        IO.puts("✓ Started distributed Erlang node: #{node()}")
      {:error, {:already_started, _pid}} ->
        IO.puts("✓ Distributed Erlang already started: #{node()}")
      error ->
        IO.puts("✗ Failed to start distributed Erlang: #{inspect(error)}")
        IO.puts("  Trying to start epmd...")
        start_epmd()
        # Try again after starting epmd
        case :net_kernel.start([unique_name]) do
          {:ok, _pid} ->
            IO.puts("✓ Started distributed Erlang node after epmd start: #{node()}")
          {:error, {:already_started, _pid}} ->
            IO.puts("✓ Distributed Erlang already started: #{node()}")
          error2 ->
            IO.puts("✗ Still failed to start distributed Erlang: #{inspect(error2)}")
            IO.puts("  Attempting to use existing node if available...")
            # If we can't start a new node, check if we're already in a distributed system
            if node() != :nonode@nohost do
              IO.puts("  Using existing distributed node: #{node()}")
            else
              IO.puts("  ✗ Cannot proceed without a distributed node")
              System.halt(1)
            end
        end
    end

    # Set cookie for Erlang distribution AFTER starting the node
    # Only set cookie if we're in a distributed system
    if node() != :nonode@nohost do
      :erlang.set_cookie(node(), String.to_atom(@cookie))
    else
      IO.puts("✗ Cannot set cookie: not in a distributed system")
      System.halt(1)
    end

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
    # Get detailed epmd information before attempting connection
    if attempt == 1 do
      case System.cmd("epmd", ["-names"]) do
        {output, 0} ->
          IO.puts("  epmd status:")
          IO.puts("    #{String.replace(output, "\n", "\n    ")}")
          # Extract port for this node
          case Regex.run(~r/name #{Regex.escape(Atom.to_string(cnode_name))} at port (\d+)/, output) do
            [_, port_str] ->
              port = String.to_integer(port_str)
              IO.puts("  CNode port from epmd: #{port}")
              # Test if port is reachable
              test_port_connectivity(port)
            _ ->
              IO.puts("  ⚠ Could not find port for #{cnode_name} in epmd output")
          end
        _ ->
          IO.puts("  ⚠ Could not query epmd")
      end
    end

    # Attempt connection
    result = :net_kernel.connect_node(cnode_name)

    case result do
      true ->
        IO.puts("✓ Connected to CNode! (attempt #{attempt})")
        true
      false ->
        # Get more details about why it failed
        nodes = :erlang.nodes()
        IO.puts("  Attempt #{attempt} failed")
        IO.puts("    Current node: #{node()}")
        IO.puts("    Connected nodes: #{inspect(nodes)}")
        IO.puts("    Target node: #{cnode_name}")

        # Check if node is visible to epmd
        case System.cmd("epmd", ["-names"]) do
          {output, 0} ->
            if String.contains?(output, Atom.to_string(cnode_name)) do
              IO.puts("    ✓ Node is registered with epmd")
            else
              IO.puts("    ✗ Node NOT found in epmd")
            end
          _ ->
            IO.puts("    ⚠ Could not check epmd")
        end

        if attempt < max_attempts do
          IO.puts("    Retrying in 1 second...")
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

  defp test_port_connectivity(port) do
    # Try to connect to the port directly to see if it's reachable
    case :gen_tcp.connect({127, 0, 0, 1}, port, [:binary, active: false], 1000) do
      {:ok, socket} ->
        IO.puts("  ✓ Port #{port} is reachable (direct TCP connection succeeded)")
        :gen_tcp.close(socket)
      {:error, reason} ->
        IO.puts("  ✗ Port #{port} is NOT reachable: #{inspect(reason)}")
        IO.puts("    This suggests a firewall or network issue")
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
