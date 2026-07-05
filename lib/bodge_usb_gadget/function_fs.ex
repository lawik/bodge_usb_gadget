# SPDX-FileCopyrightText: 2026 Lars Wikman
#
# SPDX-License-Identifier: Apache-2.0

defmodule BodgeUSBGadget.FunctionFs do
  @moduledoc """
  Custom USB device functions over FunctionFS.

  Where `BodgeUSBGadget` covers the device classes the kernel implements,
  FunctionFS is for functions the kernel does *not* know: your own protocol,
  served from Elixir. The kernel handles enumeration and the composite
  plumbing; SETUP requests for this function arrive here as events, and the
  function's endpoints are files.

  ## Lifecycle

      # 1. A gadget with an ffs function (instance name "netmd"):
      {:ok, g} = BodgeUSBGadget.define("player", %{functions: %{"ffs.netmd" => %{}}, ...})

      # 2. Mount the instance and start the function:
      :ok = FunctionFs.mount("netmd", "/dev/ffs-netmd")

      {:ok, fun} =
        FunctionFs.start_link(
          mountpoint: "/dev/ffs-netmd",
          function: %{
            interface: %{class: 0xFF},
            endpoints: [%{address: 0x01, type: :bulk}, %{address: 0x81, type: :bulk}],
            flags: [:all_ctrl_recip]
          },
          strings: ["NetMD"],
          handler: &MyProtocol.handle_setup/2
        )

      # 3. Only now can the gadget bind (FunctionFS must have its descriptors):
      :ok = BodgeUSBGadget.bind(g, udc)

  ## Control transfers: the handler

  `handler` is a 2-arity fun called in the server for each SETUP aimed at this
  function (`flags: [:all_ctrl_recip]` includes device-recipient requests):

    * IN (`setup.request_type >= 0x80`): called as `handler.(setup, nil)`;
      return `{:reply, iodata}` (truncated to `wLength`) or `:stall`.
    * OUT: the data stage (`wLength` bytes) is read first and the transfer
      thereby accepted, then `handler.(setup, data)` is called; the return
      value is ignored. (v1 limitation: OUT requests cannot be stalled.)

  A crashing handler stalls the request and the server keeps running.

  ## Everything else

  Lifecycle events are sent to `opts[:notify]` (default: the caller) as
  `{:functionfs, server, :bound | :enabled | :disabled | :unbound | :suspend |
  :resume}`. `:enabled` means the host configured the device: endpoints are
  live from that point.

  Endpoint I/O: `open_endpoint/2` opens `epN` (numbered in declaration order,
  from 1). These files *block* until the host transacts and are not pollable,
  so drive them with `read/2` and `write/2` from their own process (a `Task`
  per direction is the usual shape); each in-flight call occupies a dirty I/O
  scheduler. Unbinding the gadget unblocks them with `{:error, :eshutdown}`.
  """

  use GenServer

  alias BodgeUSBGadget.FunctionFs.Descriptors
  alias BodgeUSBGadget.Nif

  require Logger

  @typedoc "A decoded SETUP request."
  @type setup :: %{
          request_type: 0..0xFF,
          request: 0..0xFF,
          value: 0..0xFFFF,
          index: 0..0xFFFF,
          length: 0..0xFFFF
        }

  @typedoc "The control-request handler. See the moduledoc for the contract."
  @type handler :: (setup(), binary() | nil -> {:reply, iodata()} | :stall | :ok)

  @typedoc "An open endpoint file handle (see `open_endpoint/2`)."
  @opaque endpoint :: reference()

  @typedoc "One endpoint of the function, in declaration order (`ep1`, `ep2`, ...)."
  @type endpoint_spec :: %{
          required(:address) => 0..0xFF,
          required(:type) => :bulk | :interrupt | :isochronous,
          optional(:max_packet_size) => 1..0xFFFF,
          optional(:interval) => 0..0xFF
        }

  @typedoc "The function description the ep0 blobs are built from."
  @type function_spec :: %{
          required(:interface) => %{
            optional(:class) => 0..0xFF,
            optional(:subclass) => 0..0xFF,
            optional(:protocol) => 0..0xFF,
            optional(:string_index) => 0..0xFF
          },
          required(:endpoints) => [endpoint_spec()],
          optional(:flags) => [:all_ctrl_recip]
        }

  # include/uapi/linux/usb/functionfs.h event types.
  @event_names %{
    0 => :bound,
    1 => :unbound,
    2 => :enabled,
    3 => :disabled,
    4 => :setup,
    5 => :suspend,
    6 => :resume
  }

  @doc """
  Mount a FunctionFS instance (the `NAME` of an `ffs.NAME` gadget function) at
  `mountpoint`, creating the directory if needed. Needs root.
  """
  @spec mount(String.t(), Path.t()) :: :ok | {:error, term()}
  def mount(instance, mountpoint) do
    with :ok <- File.mkdir_p(mountpoint) do
      case System.cmd("mount", ["-t", "functionfs", instance, mountpoint], stderr_to_stdout: true) do
        {_out, 0} -> :ok
        {out, status} -> {:error, {:mount_failed, {status, String.trim(out)}}}
      end
    end
  end

  @doc "Unmount a FunctionFS instance."
  @spec umount(Path.t()) :: :ok | {:error, term()}
  def umount(mountpoint) do
    case System.cmd("umount", [mountpoint], stderr_to_stdout: true) do
      {_out, 0} -> :ok
      {out, status} -> {:error, {:umount_failed, {status, String.trim(out)}}}
    end
  end

  @doc """
  Start the function: opens `ep0` under `opts[:mountpoint]`, writes the
  descriptor and string blobs (built from `opts[:function]`, a
  `t:function_spec/0`, and `opts[:strings]`), and serves SETUP events with
  `opts[:handler]`. After this returns, the gadget can be bound.
  `opts[:notify]` (default: the caller) receives lifecycle events;
  `opts[:name]` optionally names the server.

  `ep0` is opened and the blobs are written before the server is started, so
  a failure (unmounted, no permissions, descriptors rejected) returns
  `{:error, reason}` without starting a process to crash a non-trapping
  caller through the link.
  """
  @spec start_link(keyword()) :: GenServer.on_start()
  def start_link(opts) do
    notify = Keyword.get(opts, :notify, self())
    handler = Keyword.fetch!(opts, :handler)
    mountpoint = Keyword.fetch!(opts, :mountpoint)
    function = Keyword.fetch!(opts, :function)
    strings = Keyword.get(opts, :strings, [])
    langid = Keyword.get(opts, :langid, 0x0409)

    with {:ok, handle} <- open_ep0(mountpoint, function, strings, langid) do
      GenServer.start_link(__MODULE__, {handle, handler, notify}, Keyword.take(opts, [:name]))
    end
  end

  @spec stop(GenServer.server()) :: :ok
  def stop(server), do: GenServer.stop(server)

  @doc """
  Open endpoint file `epN` for the function at `mountpoint`. Endpoints are
  numbered in descriptor declaration order, starting at 1. Drive the handle
  with `read/2` and `write/2`; close it with `close_endpoint/1` (a garbage
  collected handle also closes).
  """
  @spec open_endpoint(Path.t(), pos_integer()) :: {:ok, endpoint()} | {:error, atom()}
  def open_endpoint(mountpoint, n) when is_integer(n) and n >= 1 do
    Nif.open(Path.join(mountpoint, "ep#{n}"), [:rdwr])
  end

  @doc """
  Read up to `count` bytes from an endpoint. Blocks until the host transacts
  (dirty I/O scheduler); unbinding the gadget makes a blocked read return
  `{:error, :eshutdown}`.
  """
  @spec read(endpoint(), non_neg_integer()) :: {:ok, binary()} | {:error, atom()}
  def read(endpoint, count), do: Nif.read_blocking(endpoint, count)

  @doc "The write-side twin of `read/2`. Returns `{:ok, bytes_written}`."
  @spec write(endpoint(), iodata()) :: {:ok, non_neg_integer()} | {:error, atom()}
  def write(endpoint, data), do: Nif.write_blocking(endpoint, data)

  @doc "Close an endpoint handle. Idempotent; unblocks nothing by itself."
  @spec close_endpoint(endpoint()) :: :ok
  def close_endpoint(endpoint), do: Nif.close(endpoint)

  defp open_ep0(mountpoint, function, strings, langid) do
    with {:ok, handle} <- Nif.open(Path.join(mountpoint, "ep0"), [:rdwr]) do
      case blobs(handle, function, strings, langid) do
        :ok ->
          {:ok, handle}

        {:error, _} = err ->
          Nif.close(handle)
          err
      end
    end
  end

  defp blobs(handle, function, strings, langid) do
    with :ok <- write_blob(handle, Descriptors.descriptors(function)) do
      write_blob(handle, Descriptors.strings(strings, langid))
    end
  end

  # ---- server ------------------------------------------------------------

  @impl true
  def init({handle, handler, notify}) do
    # Trap exits so a crashing linked owner still runs terminate/2 (closes ep0).
    Process.flag(:trap_exit, true)
    {:ok, arm(%{handle: handle, ref: make_ref(), handler: handler, notify: notify})}
  end

  @impl true
  def handle_info({:select, _handle, ref, :ready_input}, %{ref: ref} = state) do
    # One 12-byte event per readiness cycle; re-arming immediately fires again
    # while more events are queued, and a SETUP leaves ep0 expecting its data
    # stage as the *next* I/O, so single-event reads keep the state machine
    # unambiguous.
    case Nif.read_blocking(state.handle, 12) do
      {:ok, <<setup_raw::binary-size(8), type, _pad::binary-size(3)>>} ->
        handle_event(Map.get(@event_names, type, {:unknown, type}), setup_raw, state)

      {:ok, _short} ->
        :ok

      {:error, reason} ->
        Logger.warning("BodgeUSBGadget.FunctionFs: ep0 event read failed: #{inspect(reason)}")
    end

    {:noreply, arm(state)}
  end

  def handle_info({:select, _handle, _stale_ref, _}, state), do: {:noreply, state}

  def handle_info({:EXIT, _pid, reason}, state), do: {:stop, reason, state}

  @impl true
  def terminate(_reason, state) do
    Nif.close(state.handle)
    :ok
  end

  # ---- events --------------------------------------------------------------

  defp handle_event(:setup, setup_raw, state) do
    <<request_type, request, value::little-16, index::little-16, length::little-16>> = setup_raw

    setup = %{
      request_type: request_type,
      request: request,
      value: value,
      index: index,
      length: length
    }

    if request_type >= 0x80 do
      handle_setup_in(setup, state)
    else
      handle_setup_out(setup, state)
    end
  end

  defp handle_event({:unknown, type}, _setup_raw, _state) do
    Logger.warning("BodgeUSBGadget.FunctionFs: unknown ep0 event type #{type}")
  end

  defp handle_event(event, _setup_raw, state) do
    send(state.notify, {:functionfs, self(), event})
  end

  # IN: the handler produces the data stage (or refuses); writing is the
  # response, reading in the wrong direction is the documented stall.
  defp handle_setup_in(setup, state) do
    case run_handler(state.handler, setup, nil) do
      {:reply, iodata} ->
        data = IO.iodata_to_binary(iodata)
        reply = binary_part(data, 0, min(byte_size(data), setup.length))
        checked(Nif.write_blocking(state.handle, reply), "control IN reply")

      _stall ->
        _ = Nif.read_blocking(state.handle, 1)
        :ok
    end
  end

  # OUT: reading the data stage accepts the transfer (a zero-length read acks
  # a dataless request), then the handler sees the payload.
  defp handle_setup_out(setup, state) do
    case checked(Nif.read_blocking(state.handle, setup.length), "control OUT data") do
      {:ok, data} -> run_handler(state.handler, setup, data)
      _error -> :ok
    end
  end

  defp run_handler(handler, setup, data) do
    handler.(setup, data)
  rescue
    e ->
      Logger.warning(
        "BodgeUSBGadget.FunctionFs: handler crashed on #{inspect(setup)}: #{inspect(e)}"
      )

      :stall
  end

  defp checked({:error, reason} = error, what) do
    Logger.warning("BodgeUSBGadget.FunctionFs: #{what} failed: #{inspect(reason)}")
    error
  end

  defp checked(ok, _what), do: ok

  defp write_blob(handle, blob) do
    case Nif.write(handle, blob) do
      {:ok, n} when n == byte_size(blob) -> :ok
      {:ok, n} -> {:error, {:short_blob_write, {n, byte_size(blob)}}}
      {:error, _} = err -> err
    end
  end

  defp arm(state) do
    case Nif.select_read(state.handle, state.ref) do
      :ok ->
        :ok

      {:error, reason} ->
        # The server would otherwise go silently deaf to ep0 events.
        Logger.warning("BodgeUSBGadget.FunctionFs: select_read failed: #{inspect(reason)}")
    end

    state
  end
end
