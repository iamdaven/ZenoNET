defmodule ZenonetElixir.MixProject do
  use Mix.Project

  @source_url "https://github.com/iamdaven/ZenoNET"
  @version "0.1.0"

  def project do
    [
      app: :zenonet_elixir,
      version: @version,
      elixir: "~> 1.14",
      start_permanent: Mix.env() == :prod,
      deps: deps(),
      description: "Elixir bindings for ZenoNET multiplayer networking library",
      package: package(),
      source_url: @source_url
    ]
  end

  def application do
    [
      extra_applications: [:logger]
    ]
  end

  defp deps do
    [
      {:ex_doc, "~> 0.29", only: :dev, runtime: false}
    ]
  end

  defp package do
    [
      name: :zenonet_elixir,
      files: ["lib", "native", "mix.exs", "README.md"],
      licenses: ["MIT"],
      links: %{"GitHub" => @source_url}
    ]
  end
end