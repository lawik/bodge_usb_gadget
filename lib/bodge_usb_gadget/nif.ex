# SPDX-FileCopyrightText: 2026 Lars Wikman
#
# SPDX-License-Identifier: Apache-2.0

defmodule BodgeUSBGadget.Nif do
  @moduledoc false

  # Raw fd shim for gadget-side files: FunctionFS ep0/endpoint files and
  # kernel-function chardevs (hidg, ttyGS). Internal; the public surface is
  # `BodgeUSBGadget` and `BodgeUSBGadget.FunctionFs`.
  #
  # All calls return {:error, errno_atom} on failure. read/2 and write/2 run
  # inline and assume a fast or non-blocking fd; read_blocking/2 and
  # write_blocking/2 are for endpoint files that block until the host
  # transacts (dirty I/O, fd lock released across the syscall, close deferred
  # until the call returns).

  @type handle :: reference()
  @type open_flag :: :rdonly | :wronly | :rdwr | :nonblock

  @on_load :load_nif
  @doc false
  @spec load_nif() :: :ok | {:error, {atom(), charlist()}}
  def load_nif() do
    path = :filename.join(:code.priv_dir(:bodge_usb_gadget), ~c"bodge_usb_gadget_nif")
    :erlang.load_nif(path, 0)
  end

  @spec open(binary() | charlist(), [open_flag()]) :: {:ok, handle()} | {:error, atom()}
  def open(path, flags \\ [:rdwr])
  def open(path, flags) when is_list(path), do: open(IO.iodata_to_binary(path), flags)
  def open(_path, _flags), do: :erlang.nif_error(:nif_not_loaded)

  @spec close(handle()) :: :ok
  def close(_handle), do: :erlang.nif_error(:nif_not_loaded)

  @spec read(handle(), non_neg_integer()) :: {:ok, binary()} | {:error, atom()}
  def read(_handle, _count), do: :erlang.nif_error(:nif_not_loaded)

  @spec write(handle(), iodata()) :: {:ok, non_neg_integer()} | {:error, atom()}
  def write(_handle, _data), do: :erlang.nif_error(:nif_not_loaded)

  @spec read_blocking(handle(), non_neg_integer()) :: {:ok, binary()} | {:error, atom()}
  def read_blocking(_handle, _count), do: :erlang.nif_error(:nif_not_loaded)

  @spec write_blocking(handle(), iodata()) :: {:ok, non_neg_integer()} | {:error, atom()}
  def write_blocking(_handle, _data), do: :erlang.nif_error(:nif_not_loaded)

  @spec select_read(handle(), reference()) :: :ok | {:error, atom()}
  def select_read(_h, _ref), do: :erlang.nif_error(:nif_not_loaded)

  # Test aid: the underlying integer fd, to assert descriptors are released.
  @spec fileno(handle()) :: integer() | {:error, :ebadf}
  def fileno(_handle), do: :erlang.nif_error(:nif_not_loaded)
end
