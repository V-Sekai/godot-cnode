#!/usr/bin/env elixir
# Script to capture SceneTree properties from Godot CNode

defmodule CaptureSceneTree do
  @cookie "godotcookie"

  def run do
    # Get CNode name
    cnode_name = get_cnode_name()

    IO.puts("=== Capturing SceneTree Properties ===")
    IO.puts("CNode: #{cnode_name}")
    IO.puts("")

    # Start distributed node
    start_distributed_node()

    # Set cookie
    :erlang.set_cookie(node(), String.to_atom(@cookie))

    # Connect
    case :net_kernel.connect_node(cnode_name) do
      true ->
        IO.puts("✓ Connected to CNode")
        capture_properties(cnode_name)
      _ ->
        IO.puts("✗ Failed to connect")
        System.halt(1)
    end
  end

  defp get_cnode_name do
    case System.cmd("epmd", ["-names"]) do
      {output, 0} ->
        cond do
          String.contains?(output, "godot@127.0.0.1") -> :"godot@127.0.0.1"
          String.contains?(output, "godot@localhost") -> :"godot@localhost"
          true ->
            case Regex.run(~r/name (godot@[\w\.-]+)/, output) do
              [_, name] -> String.to_atom(name)
              _ -> :"godot@127.0.0.1"
            end
        end
      _ -> :"godot@127.0.0.1"
    end
  end

  defp start_distributed_node do
    node_name = :"capture_#{System.system_time(:second)}@127.0.0.1"
    case :net_kernel.start([node_name]) do
      {:ok, _} -> IO.puts("✓ Started distributed node")
      {:error, {:already_started, _}} -> IO.puts("✓ Node already running")
      error ->
        IO.puts("✗ Failed: #{inspect(error)}")
        System.halt(1)
    end
  end

  defp capture_properties(cnode_name) do
    IO.puts("\n=== Step 1: Get SceneTree Instance ===\n")

    # Get SceneTree via the new get_scene_tree function
    case call_cnode(cnode_name, :godot, :get_scene_tree, []) do
      {:ok, {:object, class, id}} ->
        IO.puts("✓ Got SceneTree: #{class} (ID: #{id})")
        get_properties(cnode_name, id)
      {:error, reason} ->
        IO.puts("⚠ Could not get SceneTree instance: #{inspect(reason)}")
        IO.puts("  (This is expected - using class properties only)\n")
        # Continue with class properties only
        get_class_properties_only(cnode_name)
    end
  end

  defp get_properties(cnode_name, scene_tree_id) do
    IO.puts("\n=== Step 2: Get Class Properties ===\n")

    case call_cnode(cnode_name, :godot, :get_class_properties, ["SceneTree"]) do
      {:ok, properties} ->
        IO.puts("✓ Got #{length(properties)} properties")
        IO.puts("")
        IO.puts("=== SceneTree Properties ===\n")

        # Display properties
        Enum.each(properties, fn prop ->
          case prop do
            [name, type, class_name] ->
              type_str = type_to_string(type)
              class_str = if class_name != "", do: " (#{class_name})", else: ""
              IO.puts("  #{name}: #{type_str}#{class_str}")
            _ ->
              IO.puts("  #{inspect(prop)}")
          end
        end)

        # Get property values (if we have a scene_tree_id)
        if scene_tree_id do
          IO.puts("\n=== Step 3: Get Property Values ===\n")
          get_property_values(cnode_name, scene_tree_id, properties)
        else
          IO.puts("\n⚠ Skipping property values (no SceneTree instance ID)")
        end

        # Save to file
        save_to_file(properties, scene_tree_id)

      {:error, reason} ->
        IO.puts("✗ Failed: #{inspect(reason)}")
    end
  end

  defp get_class_properties_only(cnode_name) do
    IO.puts("\n=== Getting Class Properties Only ===\n")
    get_properties(cnode_name, nil)
  end

  defp get_property_values(cnode_name, object_id, properties) do
    IO.puts("Property Values and Instance IDs:\n")
    IO.puts("  [NOTE] Property values require active CNode connection")
    IO.puts("  [NOTE] Current implementation uses mock data\n")

    Enum.each(properties, fn prop ->
      case prop do
        [name, _type, class_name] ->
          case call_cnode(cnode_name, :godot, :get_property, [object_id, name]) do
            {:ok, value} ->
              value_str = format_value(value)

              # For OBJECT types, the value_str already includes the instance ID
              # Format: "ClassName (ID: instance_id)"
              class_str = if class_name != "", do: " [#{class_name}]", else: ""
              IO.puts("  #{name}#{class_str} = #{value_str}")
            {:error, reason} ->
              IO.puts("  #{name} = <not available: #{inspect(reason)}>")
          end
        _ ->
          :ok
      end
    end)
  end


  defp call_cnode(cnode_name, module, function, args) do
    # Send GenServer-style call message to CNode using global name
    # The CNode registers a global name 'godot_server' which allows us to use :erlang.send()
    # Format: {'$gen_call', {From, Tag}, {call, Module, Function, Args}}
    from = self()
    ref = make_ref()

    # Create the request tuple: {call, Module, Function, Args}
    request = {:call, module, function, args}

    # Create the GenServer call message
    message = {'$gen_call', {from, ref}, request}

    # Send message to the CNode using the registered global name
    # The CNode receives messages via ei_receive_msg() which gets distribution messages
    try do
      # The CNode must have accepted at least one connection and registered
      # the global name before we can send messages. The registration happens
      # in ei_accept() which is called after we connect. Give it time to register.
      # Also, we need to trigger the connection acceptance by the CNode.
      # The connection is established by :net_kernel.connect_node(), but the CNode
      # needs to call ei_accept() to actually accept it and register the global name.

      # Wait a bit for the CNode to accept the connection and register
      Process.sleep(500)

      # Check if global name exists (optional - for debugging)
      case :global.whereis_name({:godot_server, cnode_name}) do
        :undefined ->
          # Global name not registered yet - try anyway, might work
          :ok
        _pid ->
          # Global name exists
          :ok
      end

      # Try to send the message
      # Note: :erlang.send() should return :ok on success, or raise an exception
      # If it returns something else, that's unexpected
      :erlang.send({:godot_server, cnode_name}, message)

      # If we get here, the send succeeded
      # Wait for reply with timeout
      receive do
        {^ref, reply} ->
          # Decode the reply - CNode sends {reply, Data} or {error, Reason}
          decode_reply(reply)
        {^ref, {:error, reason}} ->
          {:error, reason}
      after
        10000 ->
          {:error, :timeout}
      end
    rescue
      ArgumentError ->
        # Global name doesn't exist - CNode needs to register it
        # This happens if the CNode hasn't accepted a connection yet
        {:error, :global_name_not_registered}
      e ->
        {:error, {:exception, e}}
    catch
      :exit, reason ->
        {:error, {:exit, reason}}
    end
  end

  defp decode_reply(reply) do
    # The CNode sends replies in format: {reply, Data} or {error, Reason}
    case reply do
      {:reply, data} ->
        {:ok, data}
      {:error, reason} ->
        {:error, reason}
      data when is_tuple(data) ->
        # Try to decode as {reply, Data} or {error, Reason}
        case data do
          {:reply, d} -> {:ok, d}
          {:error, r} -> {:error, r}
          _ -> {:ok, data}
        end
      _ ->
        {:ok, reply}
    end
  end

  defp type_to_string(type) do
    case type do
      0 -> "NIL"
      1 -> "BOOL"
      2 -> "INT"
      3 -> "FLOAT"
      4 -> "STRING"
      5 -> "VECTOR2"
      9 -> "VECTOR3"
      20 -> "COLOR"
      24 -> "OBJECT"
      27 -> "DICTIONARY"
      28 -> "ARRAY"
      _ -> "TYPE_#{type}"
    end
  end

  defp format_value(value) do
    case value do
      nil -> "nil"
      true -> "true"
      false -> "false"
      v when is_integer(v) -> "#{v}"
      v when is_float(v) -> "#{v}"
      v when is_binary(v) -> "\"#{v}\""
      v when is_atom(v) -> ":#{v}"
      v when is_list(v) -> "[#{Enum.join(Enum.map(v, &format_value/1), ", ")}]"
      {:object, class, id} -> "#{class} (ID: #{id})"
      _ -> inspect(value, limit: 50)
    end
  end

  defp save_to_file(properties, object_id) do
    timestamp = DateTime.utc_now() |> DateTime.to_iso8601()

    # Get property values with instance IDs
    property_details = Enum.map(properties, fn prop ->
      case prop do
        [name, type, class_name] ->
          type_str = type_to_string(type)
          class_str = if class_name != "", do: " (#{class_name})", else: ""

          # Try to get the value and instance ID
          value_info = if object_id do
            case call_cnode(:mock, :godot, :get_property, [object_id, name]) do
              {:ok, {:object, obj_class, id}} ->
                " = #{obj_class} (Instance ID: #{id})"
              {:ok, value} ->
                " = #{format_value(value)}"
              _ ->
                ""
            end
          else
            ""
          end

          "  - #{name}: #{type_str}#{class_str}#{value_info}"
        _ ->
          "  - #{inspect(prop)}"
      end
    end)

    content = """
    SceneTree Properties
    ====================
    Captured: #{timestamp}
    SceneTree Instance ID: #{inspect(object_id)}

    Properties with Instance IDs:
    #{Enum.join(property_details, "\n")}
    """

    filename = "scenetree_properties_#{System.system_time(:second)}.txt"
    File.write!(filename, content)
    IO.puts("\n✓ Saved to #{filename}")
  end
end

CaptureSceneTree.run()
