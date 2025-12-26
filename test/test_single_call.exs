:net_kernel.start([:"test_client_single@127.0.0.1"])
Node.set_cookie(:godotcookie)

from = self()
ref = make_ref()
gen_call = {:"$gen_call", {from, ref}, {:erlang, :node, []}}

IO.puts("Sending single GenServer call...")
:erlang.send({:test_server, :"test_cnode@127.0.0.1"}, gen_call)

IO.puts("Waiting for reply...")
receive do
  {^ref, reply} ->
    IO.puts("✓ Got reply: #{inspect(reply)}")
after
  3000 ->
    IO.puts("✗ Timeout waiting for reply")
end

System.halt(0)

