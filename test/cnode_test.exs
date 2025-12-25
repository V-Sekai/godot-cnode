Mix.install([
  {:ex_unit, "~> 1.0"}
])

defmodule GodotCNodeExUnitTest do
  use ExUnit.Case
  
  @cookie "godotcookie"
  @timeout 5000

  setup_all do
    hostname = case System.cmd("hostname", []) do
      {h, 0} -> String.trim(h)
      _ -> "127.0.0.1"
    end
    
    cnode_name = String.to_atom("godot@#{hostname}")
    
    # Start distributed Erlang node
    case :net_kernel.start([:"exunit_test@127.0.0.1"]) do
      {:ok, _pid} -> :ok
      {:error, {:already_started, _pid}} -> :ok
      error -> 
        IO.puts("Failed to start distributed Erlang: #{inspect(error)}")
        :error
    end

    # Set cookie
    :erlang.set_cookie(node(), String.to_atom(@cookie))
    
    # Connect to CNode
    max_attempts = 5
    connected = try_connect(cnode_name, max_attempts)
    
    if connected do
      {:ok, cnode_name: cnode_name}
    else
      {:error, :connection_failed}
    end
  end

  test "CNode is in connected nodes", %{cnode_name: cnode_name} do
    assert cnode_name in :erlang.nodes()
  end

  test "RPC call to CNode", %{cnode_name: cnode_name} do
    result = :rpc.call(cnode_name, :erlang, :node, [], @timeout)
    assert result != {:badrpc, _}
  end

  test "Send message to CNode", %{cnode_name: cnode_name} do
    message = {:test, :message, "from ExUnit"}
    assert :ok = send({:godot_server, cnode_name}, message)
  end

  defp try_connect(_cnode_name, 0), do: false
  
  defp try_connect(cnode_name, attempts) do
    case :net_kernel.connect_node(cnode_name) do
      true -> true
      false -> 
        Process.sleep(1000)
        try_connect(cnode_name, attempts - 1)
      :ignored -> false
    end
  end
end

ExUnit.start()
