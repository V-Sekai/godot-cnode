#!/usr/bin/env elixir

# Simple CNode integration test
# Tests GenServer-style API with a real Godot instance
# Uses direct Erlang distribution protocol to communicate with CNode

@cnode_name :"godot@127.0.0.1"
@cookie "godotcookie"
@timeout 10000

defmodule CNodeTestSimple do
  def run do
    IO.puts("=== CNode Integration Test ===")
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
        IO.puts("✓ Connected to CNode")
      false ->
        IO.puts("✗ Failed to connect to CNode")
        IO.puts("  Make sure Godot is running with the CNode GDExtension loaded")
        System.halt(1)
      :ignored ->
        IO.puts("✗ Connection to CNode ignored")
        System.halt(1)
    end

    # Wait a bit for connection to stabilize
    :timer.sleep(1000)

    # Test 1: Call get_singletons
    IO.puts("\nTest 1: get_singletons")
    test_result_1 = test_call(:godot, :get_singletons, [])
    print_result(test_result_1)

    # Test 2: Call list_classes
    IO.puts("\nTest 2: list_classes")
    test_result_2 = test_call(:godot, :list_classes, [])
    print_result(test_result_2)

    # Test 3: Call get_scene_tree_root
    IO.puts("\nTest 3: get_scene_tree_root")
    test_result_3 = test_call(:godot, :get_scene_tree_root, [])
    print_result(test_result_3)

    # Test 4: Call get_class_methods
    IO.puts("\nTest 4: get_class_methods(Node)")
    test_result_4 = test_call(:godot, :get_class_methods, ["Node"])
    print_result(test_result_4)

    # Test 5: Cast (async)
    IO.puts("\nTest 5: cast log (async)")
    test_cast(:godot, :log, ["Test log message from Elixir"])
    IO.puts("✓ Cast sent (no reply expected)")

    # Summary
    passed = Enum.count([test_result_1, test_result_2, test_result_3, test_result_4], &(&1 == :ok))
    total = 4

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

  defp test_call(module, function, args) do
    # Send GenServer-style call: {call, Module, Function, Args}
    # The CNode expects BERT-encoded messages
    message = {:call, module, function, args}
    
    try do
      # Use :rpc.call to send the message
      # Note: This might not work directly - CNode uses custom message format
      # For now, we'll try using :rpc.call with a wrapper
      result = :rpc.call(@cnode_name, :erlang, :send, [self(), message], @timeout)
      
      # Wait for reply
      receive do
        {:reply, _data} = reply ->
          {:ok, reply}
        {:error, _reason} = error ->
          {:error, error}
        other ->
          {:error, {:unexpected, other}}
      after
        @timeout ->
          {:error, :timeout}
      end
    rescue
      e ->
        {:error, {:exception, e}}
    catch
      :exit, reason ->
        {:error, {:exit, reason}}
    end
  end

  defp test_cast(module, function, args) do
    message = {:cast, module, function, args}
    :rpc.cast(@cnode_name, :erlang, :send, [self(), message])
    :ok
  end

  defp print_result({:ok, {:reply, data}}) do
    IO.puts("✓ Success")
    if is_list(data) and length(data) > 0 do
      IO.puts("  Result: #{inspect(Enum.take(data, 5))}... (#{length(data)} items)")
    else
      IO.puts("  Result: #{inspect(data)}")
    end
    :ok
  end

  defp print_result({:ok, result}) do
    IO.puts("✓ Success")
    IO.puts("  Result: #{inspect(result)}")
    :ok
  end

  defp print_result({:error, reason}) do
    IO.puts("✗ Failed: #{inspect(reason)}")
    :error
  end
end

CNodeTestSimple.run()

