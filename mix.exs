# SPDX-FileCopyrightText: 2026 Lars Wikman
#
# SPDX-License-Identifier: Apache-2.0

defmodule BodgeUSBGadget.MixProject do
  use Mix.Project

  @version "0.1.0"

  def project do
    [
      app: :bodge_usb_gadget,
      version: @version,
      elixir: "~> 1.17",
      start_permanent: Mix.env() == :prod,
      elixirc_paths: elixirc_paths(Mix.env()),
      compilers: [:elixir_make] ++ Mix.compilers(),
      make_targets: ["all"],
      make_clean: ["clean"],
      deps: deps(),
      name: "BodgeUSBGadget",
      description: "Be a USB device from Elixir on Linux: configfs gadgets and FunctionFS",
      docs: docs(),
      package: package(),
      aliases: aliases(),
      dialyzer: dialyzer()
    ]
  end

  def application do
    [
      extra_applications: [:logger]
    ]
  end

  defp elixirc_paths(:test), do: ["lib", "test/support"]
  defp elixirc_paths(_), do: ["lib"]

  def docs do
    [
      main: "readme",
      source_url: "https://github.com/lawik/bodge_usb_gadget",
      source_ref: "v#{@version}",
      extras: ["README.md", "CHANGELOG.md"]
    ]
  end

  def package do
    [
      name: :bodge_usb_gadget,
      # Hex's default file set excludes c_src/ and the Makefile, without which
      # the package cannot compile.
      files: [
        "lib",
        "c_src",
        "Makefile",
        "mix.exs",
        "README.md",
        "LICENSE.md",
        "LICENSES",
        "REUSE.toml",
        "CHANGELOG.md"
      ],
      licenses: ["Apache-2.0"],
      links: %{"GitHub" => "https://github.com/lawik/bodge_usb_gadget"}
    ]
  end

  def aliases do
    [
      check: [
        "hex.audit",
        "compile --warnings-as-errors --force",
        "format --check-formatted",
        "credo --strict",
        "deps.unlock --check-unused",
        "spellweaver.check",
        "dialyzer"
      ],
      precommit: [
        "hex.audit",
        "compile --warnings-as-errors --force",
        "format",
        "credo --strict",
        "deps.unlock --unused",
        "spellweaver.check",
        "dialyzer",
        "test"
      ]
    ]
  end

  def cli do
    [
      preferred_envs: [precommit: :test]
    ]
  end

  def dialyzer do
    [
      plt_add_apps: [:mix],
      ignore_warnings: ".dialyzer_ignore.exs"
    ]
  end

  defp deps do
    [
      {:elixir_make, "~> 0.8", runtime: false},
      {:nstandard, "~> 0.5", only: [:dev, :test], runtime: false},
      # Host side of the same VM-backed integration tests: this library defines
      # a gadget and bodge_usb drives it from the host end of dummy_hcd.
      {:bodge_usb, "~> 0.1.1", only: [:test]},
      {:ex_doc, "~> 0.40", only: [:dev, :test], runtime: false},
      {:dialyxir, "~> 1.4", only: [:dev, :test], runtime: false},
      {:credo, "~> 1.7", only: [:dev, :test], runtime: false},
      {:spellweaver, "~> 0.1.8", only: [:dev, :test], runtime: false}
    ]
  end
end
