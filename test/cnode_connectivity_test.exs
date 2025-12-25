#!/usr/bin/env elixir

# Basic CNode connectivity test
# Verifies that the CNode is running and can be connected to
# Full API testing requires a wrapper module (see README)

@cnode_name :"godot@127.0.0.1"
@cookie "godotcookie"

defmodule CNodeConnectivityTest do
  def run do
    IO.puts("=== CNode Connectivity Test ===")
    IO.puts("CNode: #{@cnode_name}")
    IO.puts("Cookie: #{@cookie}")
    IO.puts("")

    # Set cookie for Erlang distribution
    :erlang.set_cookie(node(), String.to_atom(@cookie))

    # Start distributed Erlang
    case :net_kernel.start([:"test@127.0.0.1"]) do
      {:ok, _pid} ->
        IO.puts("✓ Started distributed Erlang node")
      {:error, {:already_started, _pid}} ->
        IO.puts("✓ Distributed Erlang already started")
      error ->
        IO.puts("✗ Failed to start distributed Erlang: #{inspect(error)}")
        System.halt(1)
    end

    # Try to connect to CNode
    IO.puts("Connecting to CNode: #{@cnode_name}")
    case :net_kernel.connect_node(@cnode_name) do
      true ->
        IO.puts("✓ Successfully connected to CNode!")
        IO.puts("")
        IO.puts("Note: Full API testing requires sending BERT-encoded messages")
        IO.puts("in the format {call, Module, Function, Args} or {cast, Module, Function, Args}.")
        IO.puts("See README for details on using the GenServer-style API.")
        System.halt(0)
      false ->
        IO.puts("✗ Failed to connect to CNode")
        IO.puts("")
        IO.puts("Possible issues:")
        IO.puts("  1. Godot is not running")
        IO.puts("  2. CNode GDExtension is not loaded")
        IO.puts("  3. CNode is not published (check Godot console)")
        IO.puts("  4. Cookie mismatch (expected: #{@cookie})")
        IO.puts("  5. Network/firewall issues")
        System.halt(1)
      :ignored ->
        IO.puts("✗ Connection to CNode ignored")
        IO.puts("  This usually means the node name is invalid or already connected")
        System.halt(1)
    end
  end
end

CNodeConnectivityTest.run()

