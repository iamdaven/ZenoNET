defmodule ZenonetElixir do
  @moduledoc """
  Elixir bindings for ZenoNET multiplayer networking library.
  
  ZenoNET is a C-based networking library designed for multiplayer games,
  providing both server and client functionality with support for rooms,
  rooms, and real-time communication.
  
  ## Features
  
  - TCP/UDP server and client
  - Room-based messaging
  - Binary protocol (ZNP) for efficient serialization
  - Callback-based event handling
  - Cross-platform support (Windows, Linux, macOS)
  
  ## Example Usage
  
  ### Server
  
      {:ok, server} = ZenonetElixir.Server.start(7777)
      ZenonetElixir.Server.on_connect(server, fn conn -> 
        IO.puts("Client connected: #{conn.id}")
      end)
      ZenonetElixir.Server.on_data(server, fn conn, data -> 
        IO.puts("Received: #{data}")
        ZenonetElixir.Server.broadcast(server, "Hello everyone!")
      end)
  
  ### Client
  
      {:ok, client} = ZenonetElixir.Client.start_link("localhost", 7777, "Player1")
      ZenonetElixir.Client.on_connect(client, fn -> 
        IO.puts("Connected!")
        ZenonetElixir.Client.join_room(client, "game_room")
      end)
      ZenonetElixir.Client.on_data(client, fn data -> 
        IO.puts("Received: #{data}")
      end)
      ZenonetElixir.Client.send(client, "{\"action\": \"move\", \"x\": 10, \"y\": 20}")
  """

  @type server_ref :: reference()
  @type client_ref :: reference()
  @type connection :: %{id: String.t(), name: String.t(), addr: String.t(), port: non_neg_integer()}
  @type data :: String.t() | map()

  @doc """
  Returns the version of the ZenoNET library.
  """
  @spec version() :: String.t()
  def version do
    Native.version()
  end

  @doc """
  Returns the current Unix timestamp.
  """
  @spec timestamp() :: non_neg_integer()
  def timestamp do
    Native.timestamp()
  end
end