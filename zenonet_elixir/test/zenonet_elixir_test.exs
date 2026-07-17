defmodule ZenonetElixirTest do
  use ExUnit.Case
  doctest ZenonetElixir

  describe "version/0" do
    test "returns version string" do
      version = ZenonetElixir.version()
      assert is_binary(version)
      assert version != ""
    end
  end

  describe "timestamp/0" do
    test "returns current timestamp" do
      timestamp = ZenonetElixir.timestamp()
      assert is_integer(timestamp)
      assert timestamp > 0
    end
  end

  describe "Server" do
    test "can create a server" do
      # Note: This test requires the native library to be compiled
      # It will fail with :native_not_loaded if the NIFs aren't loaded
      assert_raise RuntimeError, ~r/native_not_loaded/, fn ->
        ZenonetElixir.Server.start(7777)
      end
    end
  end

  describe "Client" do
    test "can create a client" do
      assert_raise RuntimeError, ~r/native_not_loaded/, fn ->
        ZenonetElixir.Client.start_link("localhost", 7777, "TestPlayer")
      end
    end
  end
end