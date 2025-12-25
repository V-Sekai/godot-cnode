defmodule GodotCNodeSimpleTest do
  @cookie "godotcookie"
  @timeout 5000

  def run do
    hostname = case System.cmd("hostname", []) do
      {h, 0} -> String.trim(h)
      _ -> "127.0.0.1"
    end
    
    cnode_name = String.to_atom("godot@#{hostname}")
    
    IO.puts("=== Godot CNode Simple Test ===")
    IO.puts("CNode: #{cnode_name}")
    IO.puts("Cookie: #{@cookie}")
    IO.puts("")

    # Start distributed Erlang node FIRST
    case :net_kernel.start([:"simple_test@127.0.0.1"]) do
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
    
    max_attempts = 5
    if try_connect_attempt(cnode_name, 1, max_attempts) do
      IO.puts("\n✓ Successfully connected to CNode!")
      IO.puts("  Connected nodes: #{inspect(:erlang.nodes())}")
    else
      IO.puts("\n✗ Failed to connect to CNode after #{max_attempts} attempts")
      System.halt(1)
    end
  end

  defp try_connect_attempt(_cnode_name, attempt, max_attempts) when attempt > max_attempts do
    false
  end

  defp try_connect_attempt(cnode_name, attempt, max_attempts) do
    case :net_kernel.connect_node(cnode_name) do
      true ->
        IO.puts("  ✓ Connected! (attempt #{attempt})")
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
        IO.puts("  ✗ Connection ignored (attempt #{attempt})")
        false
    end
  end
end

GodotCNodeSimpleTest.run()
