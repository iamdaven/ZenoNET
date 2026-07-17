defmodule ZenonetElixir.Native do
  @moduledoc """
  Native bindings to ZenoNET C library via Rust NIFs.
  """

  # Server functions
  def server_create(port) do
    :erlang.nif_error(:native_not_loaded)
  end

  def server_create_host(host, port) do
    :erlang.nif_error(:native_not_loaded)
  end

  def server_start(server_ref) do
    :erlang.nif_error(:native_not_loaded)
  end

  def server_stop(server_ref) do
    :erlang.nif_error(:native_not_loaded)
  end

  def server_broadcast(server_ref, data) do
    :erlang.nif_error(:native_not_loaded)
  end

  def server_connection_count(server_ref) do
    :erlang.nif_error(:native_not_loaded)
  end

  def server_on_connect(server_ref, pid, function) do
    :erlang.nif_error(:native_not_loaded)
  end

  def server_on_disconnect(server_ref, pid, function) do
    :erlang.nif_error(:native_not_loaded)
  end

  def server_on_data(server_ref, pid, function) do
    :erlang.nif_error(:native_not_loaded)
  end

  # Client functions
  def client_new do
    :erlang.nif_error(:native_not_loaded)
  end

  def client_connect(client_ref, host, port) do
    :erlang.nif_error(:native_not_loaded)
  end

  def client_connect_name(client_ref, host, port, name) do
    :erlang.nif_error(:native_not_loaded)
  end

  def client_disconnect(client_ref) do
    :erlang.nif_error(:native_not_loaded)
  end

  def client_send(client_ref, data) do
    :erlang.nif_error(:native_not_loaded)
  end

  def client_join_room(client_ref, room) do
    :erlang.nif_error(:native_not_loaded)
  end

  def client_leave_room(client_ref) do
    :erlang.nif_error(:native_not_loaded)
  end

  def client_is_connected(client_ref) do
    :erlang.nif_error(:native_not_loaded)
  end

  def client_on_connect(client_ref, pid, function) do
    :erlang.nif_error(:native_not_loaded)
  end

  def client_on_disconnect(client_ref, pid, function) do
    :erlang.nif_error(:native_not_loaded)
  end

  def client_on_data(client_ref, pid, function) do
    :erlang.nif_error(:native_not_loaded)
  end

  # Packet functions
  def packet_create(packet_type, data) do
    :erlang.nif_error(:native_not_loaded)
  end

  def packet_create_with_room(packet_type, data, room) do
    :erlang.nif_error(:native_not_loaded)
  end

  # Utility functions
  def version do
    :erlang.nif_error(:native_not_loaded)
  end

  def timestamp do
    :erlang.nif_error(:native_not_loaded)
  end
end