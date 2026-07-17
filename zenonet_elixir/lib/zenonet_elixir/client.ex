defmodule ZenonetElixir.Client do
  @moduledoc """
  ZenoNET client functionality.
  
  Provides a high-level interface for connecting to game servers,
  sending/receiving data, and managing rooms.
  """

  use GenServer
  require Logger

  alias ZenonetElixir.Native

  @type t :: GenServer.server()
  @type connection_info :: %{
    id: String.t(),
    name: String.t(),
    connected: boolean()
  }

  @doc """
  Starts a ZenoNET client and connects to a server.
  
  ## Examples
  
      {:ok, client} = ZenonetElixir.Client.start_link("localhost", 7777, "Player1")
  """
  @spec start_link(host :: String.t(), port :: non_neg_integer(), name :: String.t()) :: 
    {:ok, pid()} | {:error, term()}
  def start_link(host, port, name \\ "Player") when is_binary(host) and is_integer(port) do
    GenServer.start_link(__MODULE__, {host, port, name})
  end

  @doc """
  Disconnects the client from the server.
  """
  @spec disconnect(client :: t()) :: :ok
  def disconnect(client) do
    GenServer.cast(client, :disconnect)
  end

  @doc """
  Sends data to the server.
  """
  @spec send(client :: t(), data :: String.t() | map()) :: :ok | {:error, term()}
  def send(client, data) when is_binary(data) do
    GenServer.call(client, {:send, data})
  end

  def send(client, data) when is_map(data) do
    send(client, Jason.encode!(data))
  end

  @doc """
  Joins a room on the server.
  """
  @spec join_room(client :: t(), room :: String.t()) :: :ok | {:error, term()}
  def join_room(client, room) when is_binary(room) do
    GenServer.call(client, {:join_room, room})
  end

  @doc """
  Leaves the current room.
  """
  @spec leave_room(client :: t()) :: :ok | {:error, term()}
  def leave_room(client) do
    GenServer.call(client, :leave_room)
  end

  @doc """
  Returns true if the client is connected to the server.
  """
  @spec connected?(client :: t()) :: boolean()
  def connected?(client) do
    GenServer.call(client, :connected?)
  end

  @doc """
  Registers a callback for when the client connects to the server.
  """
  @spec on_connect(client :: t(), callback :: (-> any())) :: :ok
  def on_connect(client, callback) when is_function(callback, 0) do
    GenServer.cast(client, {:register_callback, :connect, callback})
  end

  @doc """
  Registers a callback for when the client disconnects from the server.
  """
  @spec on_disconnect(client :: t(), callback :: (-> any())) :: :ok
  def on_disconnect(client, callback) when is_function(callback, 0) do
    GenServer.cast(client, {:register_callback, :disconnect, callback})
  end

  @doc """
  Registers a callback for when data is received from the server.
  
  The callback receives the data string.
  """
  @spec on_data(client :: t(), callback :: (String.t() -> any())) :: :ok
  def on_data(client, callback) when is_function(callback, 1) do
    GenServer.cast(client, {:register_callback, :data, callback})
  end

  # GenServer callbacks

  @impl true
  def init({host, port, name}) do
    {:ok, client_ref} = Native.client_new()
    
    # Set up callbacks storage
    callbacks = %{
      connect: [],
      disconnect: [],
      data: []
    }
    
    state = %{
      native: client_ref,
      host: host,
      port: port,
      name: name,
      callbacks: callbacks,
      connected: false
    }
    
    # Attempt to connect
    case Native.client_connect_name(client_ref, host, port, name) do
      true ->
        Logger.info("Connected to #{host}:#{port}")
        {:ok, %{state | connected: true}}
      false ->
        Logger.error("Failed to connect to #{host}:#{port}")
        {:ok, state}
    end
  end

  @impl true
  def handle_call({:send, data}, _from, state) do
    if state.connected do
      case Native.client_send(state.native, data) do
        :ok -> {:reply, :ok, state}
        {:error, reason} -> {:reply, {:error, reason}, state}
      end
    else
      {:reply, {:error, :not_connected}, state}
    end
  end

  @impl true
  def handle_call({:join_room, room}, _from, state) do
    if state.connected do
      case Native.client_join_room(state.native, room) do
        :ok -> {:reply, :ok, state}
        {:error, reason} -> {:reply, {:error, reason}, state}
      end
    else
      {:reply, {:error, :not_connected}, state}
    end
  end

  @impl true
  def handle_call(:leave_room, _from, state) do
    if state.connected do
      case Native.client_leave_room(state.native) do
        :ok -> {:reply, :ok, state}
        {:error, reason} -> {:reply, {:error, reason}, state}
      end
    else
      {:reply, {:error, :not_connected}, state}
    end
  end

  @impl true
  def handle_call(:connected?, _from, state) do
    {:reply, state.connected, state}
  end

  @impl true
  def handle_cast(:disconnect, state) do
    if state.connected do
      Native.client_disconnect(state.native)
      Logger.info("Disconnected from #{state.host}:#{state.port}")
    end
    {:noreply, %{state | connected: false}}
  end

  @impl true
  def handle_cast({:register_callback, type, callback}, state) do
    callbacks = Map.update(state.callbacks, type, [callback], &[callback | &1])
    {:noreply, %{state | callbacks: callbacks}}
  end

  @impl true
  def terminate(_reason, state) do
    if state.connected do
      Native.client_disconnect(state.native)
    end
    :ok
  end
end