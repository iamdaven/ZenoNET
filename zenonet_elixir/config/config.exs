# Configure your application here
import Config

config :zenonet_elixir,
  # Library configuration
  native_lib: "zenonet_native",
  default_port: 7777,
  max_connections: 256,
  max_rooms: 128

# Import environment specific config
import_config "#{Mix.env()}.exs"