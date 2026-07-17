# ZenoNET Elixir Server Example
# Run with: mix run examples/server_example.exs

# Start the application
Application.ensure_all_started(:zenonet_elixir)

# Create a server on port 7777
{:ok, server} = ZenonetElixir.Server.start(7777)
IO.puts("Server started on port 7777")

# Register callbacks
ZenonetElixir.Server.on_connect(server, fn conn ->
  IO.puts("[+] Client connected: #{conn.id} (#{conn.name}) from #{conn.addr}:#{conn.port}")
end)

ZenonetElixir.Server.on_disconnect(server, fn conn ->
  IO.puts("[-] Client disconnected: #{conn.id} (#{conn.name})")
end)

ZenonetElixir.Server.on_data(server, fn conn, data ->
  IO.puts("[<] Received from #{conn.name}: #{data}")
  
  # Echo the data back to all clients
  response = Jason.encode!(%{
    from: conn.name,
    message: data,
    timestamp: System.system_time(:second)
  })
  
  ZenonetElixir.Server.broadcast(server, response)
end)

# Keep the server running
IO.puts("Press Ctrl+C to stop the server")
Process.sleep(:infinity)