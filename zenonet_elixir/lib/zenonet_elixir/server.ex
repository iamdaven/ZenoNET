defmodule ZenonetElixir.Server do
  @moduledoc """
  ZenoNET server functionality.
  
  Provides a high-level interface for creating and managing game servers
  with support for multiple clients, rooms, and real-time messaging.
  """

  use GenServer
  require Logger

  alias ZenonetElixir.Native

  @type t :: GenServer.server()
  @type connection :: %{
    id: String.t(),
    name: String.t(),
    addr: String.t(),
    port: non_neg_integer()
  }

  @doc """
  Starts a ZenoNET server on the specified port.
  
  ## Examples
  
      {:ok, server} = ZenonetElixir.Server.start(7777)
  """
  @spec start(port :: non_neg_integer()) :: {:ok, pid()} | {:error, term()}
  def start(port) when is_integer(port) and port > 0 do
    GenServer.start_link(__MODULE__, port, name: via_tuple(port))
  end

  @doc """
  Starts a server bound to a specific host and port.
  """
  @spec start_host(host :: String.t(), port :: non_neg_integer()) :: {:ok, pid()} | {:error, term()}
  def start_host(host, port) when is_binary(host) and is_integer(port) do
    GenServer.start_link(__MODULE__, {:host, host, port}, name: via_tuple({host, port}))
  end

  @doc """
  Stops the server.
  """
  @spec stop(server :: t()) :: :ok
  def stop(server) do
    GenServer.stop(server)
  end

  @doc """
  Broadcasts data to all connected clients.
  """
  @spec broadcast(server :: t(), data :: String.t() | map()) :: :ok
  def broadcast(server, data) when is_binary(data) do
    GenServer.cast(server, {:broadcast, data})
  end

  def broadcast(server, data) when is_map(data) do
    broadcast(server, Jason.encode!(data))
  end

  @doc """
  Returns the number of connected clients.
  """
  @spec connection_count(server :: t()) :: non_neg_integer()
  def connection_count(server) do
    GenServer.call(server, :connection_count)
  end

  @doc """
  Registers a callback for when a client connects.
  
  The callback receives a connection map with `:id`, `:name`, `:addr`, and `:port` keys.
  """
  @spec on_connect(server :: t(), callback :: (connection -> any())) :: :ok
  def on_connect(server, callback) when is_function(callback, 1) do
    GenServer.cast(server, {:register_callback, :connect, callback})
  end

  @doc """
  Registers a callback for when a client disconnects.
  """
  @spec on_disconnect(server :: t(), callback :: (connection -> any())) :: :ok
  def on_disconnect(server, callback) when is_function(callback, 1) do
    GenServer.cast(server, {:register_callback, :disconnect, callback})
  end

  @doc """
  Registers a callback for when data is received from a client.
  
  The callback receives the connection map and the data string.
  """
  @spec on_data(server :: t(), callback :: (connection, String.t() -> any())) :: :ok
  def on_data(server, callback) when is_function(callback, 2) do
    GenServer.cast(server, {:register_callback, :data, callback})
  end

  @doc """
  Sends data to a specific client by ID.
  """
  @spec send_to(server :: t(), client_id :: String.t(), data :: String.t() | map()) :: :ok
  def send_to(server, client_id, data) when is_binary(data) do
    GenServer.cast(server, {:send_to, client_id, data})
  end

  def send_to(server, client_id, data) when is_map(data) do
    send_to(server, client_id, Jason.encode!(data))
  end

  # GenServer callbacks

  @impl true
  def init(port) when is_integer(port) do
    {:ok, server_ref} = Native.server_create(port)
    {:ok, %{native: server_ref, port: port, callbacks: %{}}}
  end

  @impl true
  def init({:host, host, port}) do
    {:ok, server_ref} = Native.server_create_host(host, port)
    {:ok, %{native: server_ref, host: host, port: port, callbacks: %{}}}
  end

  @impl true
  def handle_cast({:broadcast, data}, state) do
    Native.server_broadcast(state.native, data)
    {:noreply, state}
  end

  @impl true
  def handle_cast({:register_callback, type, callback}, state) do
    callbacks = Map.put(state.callbacks, type, callback)
    {:noreply, %{state | callbacks: callbacks}}
  end

  @impl true
  def handle_cast({:send_to, _client_id, _data}, state) do
    # TODO: Implement send_to using connection lookup
    {:noreply, state}
  end

  @impl true
  def handle_call(:connection_count, _from, state) do
    count = Native.server_connection_count(state.native)
    {:reply, count, state}
  end

  @impl true
  def terminate(_reason, state) do
    Native.server_stop(state.native)
    :ok
  end

  defp via_tuple(port) when is_integer(port), do: {:via, Registry, {ZenoNET.Registry, "server_#{port}"}}
  defp via_tuple({host, port}), do: {:via, Registry, {ZenoNET.Registry, "server_#{host}_#{port}"}}
end