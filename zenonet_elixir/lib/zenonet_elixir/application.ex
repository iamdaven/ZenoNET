defmodule ZenonetElixir.Application do
  @moduledoc false
  use Application

  def start(_type, _args) do
    children = [
      {Registry, keys: :unique, name: ZenonetElixir.Registry}
    ]

    opts = [strategy: :one_for_one, name: ZenonetElixir.Supervisor]
    Supervisor.start_link(children, opts)
  end
end