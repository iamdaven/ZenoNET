# ZenoNET Elixir Client Example
# Run with: mix run examples/client_example.exs

# Start the application
Application.ensure_all_started(:zenonet_elixir)

# Create a client and connect to the server
{:ok, client} = ZenonetElixir.Client.start_link("localhost", 7777, "Player#{:rand.uniform(1000)}")
IO.puts("Connecting to server...")

# Register callbacks
ZenonetElixir.Client.on_connect(client, fn ->
  IO.puts("[+] Connected to server!")
  
  # Join a room
  ZenonetElixir.Client.join_room(client, "game_room")
  
  # Send a message
  ZenonetElixir.Client.send(client, %{
    type: "join",
    player: "Player#{:rand.uniform(1000)}",
    message: "Hello everyone!"
  })
end)

ZenonetElixir.Client.on_disconnect(client, fn ->
  IO.puts("[-] Disconnected from server")
end)

ZenonetElixir.Client.on_data(client, fn data ->
  IO.puts("[<] Received: #{data}")
  
  # Parse and display the message
  case Jason.decode(data) do
    {:ok, %{"from" => from, "message" => msg}} ->
      IO.puts("  From #{from}: #{msg}")
    _ ->
      :ok
  end
end)

# Keep the client running
IO.puts("Press Ctrl+C to disconnect")
Process.sleep(:infinity)