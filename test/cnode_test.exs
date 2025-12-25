#!/usr/bin/env elixir

# Basic CNode integration test
# Tests GenServer-style API with a real Godot instance

Mix.install([
  {:ex_unit, "~> 1.0"}
])

defmodule CNodeTest do
  @moduledoc """
  Integration tests for Godot CNode GenServer API.
  Tests basic CNode operations with a real Godot instance.
  """

  @cnode_name :"godot@127.0.0.1"
  @cookie "godotcookie"
  @timeout 5000

  defp connect_to_cnode do
    # Set cookie for Erlang distribution
    :erlang.set_cookie(node(), String.to_atom(@cookie))

    # Try to connect to CNode
    case :net_kernel.connect_node(@cnode_name) do
      true ->
        IO.puts("✓ Connected to CNode: #{@cnode_name}")
        true

      false ->
        IO.puts("✗ Failed to connect to CNode: #{@cnode_name}")
        false

      :ignored ->
        IO.puts("✗ Connection to CNode ignored: #{@cnode_name}")
        false
    end
  end

  defp call_cnode(module, function, args) do
    # Send GenServer-style call: {call, Module, Function, Args}
    # We need to send a BERT-encoded message directly to the CNode
    # The CNode expects: {call, Module, Function, Args}
    try do
      # Use :rpc.call with the proper format
      # The CNode processes messages in the format {call, Module, Function, Args}
      message = {:call, module, function, args}
      :rpc.call(@cnode_name, :erlang, :send, [self(), message], @timeout)
      
      # Wait for reply
      receive do
        {:reply, result} -> {:reply, result}
        {:error, reason} -> {:error, reason}
        other -> {:error, {:unexpected_reply, other}}
      after
        @timeout -> {:error, :timeout}
      end
    rescue
      e ->
        IO.puts("RPC call error: #{inspect(e)}")
        {:error, :rpc_failed}
    catch
      :exit, reason ->
        IO.puts("RPC call exit: #{inspect(reason)}")
        {:error, :rpc_exit}
    end
  end

  defp cast_cnode(module, function, args) do
    # Send GenServer-style cast: {cast, Module, Function, Args}
    message = {:cast, module, function, args}
    :rpc.cast(@cnode_name, :erlang, :send, [self(), message])
  end

  # Test functions
  def test_connection do
    connect_to_cnode()
  end

  def test_get_singletons do
    IO.puts("\nTest: get_singletons")
    result = call_cnode(:godot, :get_singletons, [])
    IO.inspect(result, label: "Result")
    case result do
      {:reply, _singletons} -> :ok
      {:error, _reason} -> :error
      _ -> :unknown
    end
  end

  def test_list_classes do
    IO.puts("\nTest: list_classes")
    result = call_cnode(:godot, :list_classes, [])
    IO.inspect(result, label: "Result")
    case result do
      {:reply, classes} when is_list(classes) ->
        IO.puts("✓ Found #{length(classes)} classes")
        :ok
      {:error, reason} ->
        IO.puts("✗ Error: #{inspect(reason)}")
        :error
      _ ->
        :unknown
    end
  end

  def test_get_scene_tree_root do
    IO.puts("\nTest: get_scene_tree_root")
    result = call_cnode(:godot, :get_scene_tree_root, [])
    IO.inspect(result, label: "Result")
    case result do
      {:reply, {:object, _class, _id}} -> :ok
      {:error, _reason} -> :error
      _ -> :unknown
    end
  end

  def test_get_class_methods do
    IO.puts("\nTest: get_class_methods(Node)")
    result = call_cnode(:godot, :get_class_methods, ["Node"])
    IO.inspect(result, label: "Result")
    case result do
      {:reply, methods} when is_list(methods) ->
        IO.puts("✓ Found #{length(methods)} methods")
        :ok
      {:error, reason} ->
        IO.puts("✗ Error: #{inspect(reason)}")
        :error
      _ ->
        :unknown
    end
  end

  def test_cast_async do
    IO.puts("\nTest: cast (async)")
    cast_cnode(:godot, :call_method, [0, "get_name", []])
    IO.puts("✓ Cast sent (no reply expected)")
    :ok
  end

  def run_all_tests do
    IO.puts("=== CNode Integration Tests ===")
    IO.puts("CNode: #{@cnode_name}")
    IO.puts("Cookie: #{@cookie}")
    IO.puts("")

    unless test_connection() do
      IO.puts("✗ Cannot connect to CNode. Skipping tests.")
      System.halt(1)
    end

    results = [
      test_get_singletons(),
      test_list_classes(),
      test_get_scene_tree_root(),
      test_get_class_methods(),
      test_cast_async()
    ]

    passed = Enum.count(results, &(&1 == :ok))
    total = length(results)

    IO.puts("\n=== Test Results ===")
    IO.puts("Passed: #{passed}/#{total}")

    if passed == total do
      IO.puts("✓ All tests passed!")
      System.halt(0)
    else
      IO.puts("✗ Some tests failed")
      System.halt(1)
    end
  end
end

CNodeTest.run_all_tests()

