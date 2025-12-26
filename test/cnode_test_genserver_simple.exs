node_name = :"genserver_test_#{System.system_time(:second)}@127.0.0.1"
:net_kernel.start([node_name])
:erlang.set_cookie(node(), :godotcookie)

case :net_kernel.connect_node(:"godot@127.0.0.1") do
  true ->
    IO.puts("✓ Connected to CNode")
    from = self()
    ref = make_ref()
    gen_call = {'$gen_call', {from, ref}, {:erlang, :node, []}}
    :erlang.send({:godot_server, :"godot@127.0.0.1"}, gen_call)

    receive do
      {^ref, reply} ->
        IO.puts("✓ GenServer RPC works! Reply: #{inspect(reply)}")
      other ->
        IO.puts("✗ Unexpected reply: #{inspect(other)}")
    after
      5000 ->
        IO.puts("✗ Timeout waiting for reply")
    end
  _ ->
    IO.puts("✗ Failed to connect")
end
