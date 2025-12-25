defmodule GodotCNodeConnectivityTest do
  @cookie "godotcookie"

  def run do
    hostname = case System.cmd("hostname", []) do
      {h, 0} -> String.trim(h)
      _ -> "127.0.0.1"
    end
    
    cnode_name = String.to_atom("godot@#{hostname}")
    
    IO.puts("=== Godot CNode Connectivity Test ===")
    IO.puts("CNode: #{cnode_name}")
    IO.puts("Cookie: #{@cookie}")
    IO.puts("")

    # Start distributed Erlang node FIRST
    case :net_kernel.start([:"connectivity_test@127.0.0.1"]) do
      {:ok, _pid} ->
        IO.puts("✓ Started distributed Erlang node")
      {:error, {:already_started, _pid}} ->
        IO.puts("✓ Distributed Erlang already started")
      error ->
        IO.puts("✗ Failed to start distributed Erlang: #{inspect(error)}")
        System.halt(1)
    end

    # Set cookie for Erlang distribution AFTER starting the node
    :erlang.set_cookie(node(), String.to_atom(@cookie))
    
    IO.puts("\nConnecting to CNode: #{cnode_name}")
    
    max_attempts = 10
    case try_connect(cnode_name, max_attempts) do
      true ->
        IO.puts("✓ Successfully connected to CNode!")
        IO.puts("  Connected nodes: #{inspect(:erlang.nodes())}")
        IO.puts("\n✓ Connectivity test passed!")
        0
      false ->
        IO.puts("✗ Failed to connect to CNode after #{max_attempts} attempts")
        IO.puts("\nTroubleshooting:")
        IO.puts("  1. Make sure Godot is running with the CNode GDExtension loaded")
        IO.puts("  2. Check the CNode name in Godot output (should match: #{cnode_name})")
        IO.puts("  3. Verify epmd can see the node: epmd -names")
        IO.puts("  4. Check cookie matches (expected: #{@cookie})")
        1
    end
  end

  defp try_connect(_cnode_name, 0), do: false

  defp try_connect(cnode_name, attempts) do
    case :net_kernel.connect_node(cnode_name) do
      true ->
        true
      false ->
        if attempts > 1 do
          IO.puts("  Connection failed, retrying... (#{attempts - 1} attempts remaining)")
          Process.sleep(1000)
          try_connect(cnode_name, attempts - 1)
        else
          false
        end
      :ignored ->
        IO.puts("  Connection ignored (node might not be registered with epmd)")
        false
    end
  end
end

exit_code = GodotCNodeConnectivityTest.run()
System.halt(exit_code)
