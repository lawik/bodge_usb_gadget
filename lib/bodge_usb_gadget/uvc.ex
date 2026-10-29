# SPDX-FileCopyrightText: 2026 Lars Wikman
#
# SPDX-License-Identifier: Apache-2.0

defmodule BodgeUSBGadget.UVC do
  @moduledoc """
  A webcam: the kernel's USB Video Class function (`usb_f_uvc`), fed frames
  from Elixir.

  The kernel does the USB side of the camera but leaves two things to a
  program on the device, through the function's V4L2 output device: the
  class's control requests, in which the host picks a format, size and frame
  rate (probe and commit), and the frames. This server runs a small helper
  (`priv/uvc_gadget`) that does both. It answers the host from the formats
  the device was made with and queues the frames it is sent.

      formats = [mjpeg: [{1280, 720}, {640, 480}], yuyv: [{640, 480}]]

      {:ok, g} =
        BodgeUSBGadget.define("webcam", %{
          vendor_id: 0x1D6B,
          product_id: 0x0102,
          strings: %{manufacturer: "bodge", product: "Webcam", serialnumber: "1"},
          functions: %{"uvc.cam" => UVC.function(formats)},
          configs: %{"c.1" => %{configuration: "webcam", max_power: 500, functions: ["uvc.cam"]}}
        })

      :ok = BodgeUSBGadget.bind(g)
      {:ok, device} = BodgeUSBGadget.device_node(g, "uvc.cam")
      {:ok, uvc} = UVC.start_link(device: device)

  The kernel keeps a bound webcam off the bus until a program serves it, so
  the host sees the camera from when the server starts until it stops. The
  server sends `opts[:notify]` (default: the caller):

    * `{:uvc, uvc, :connected}` - a host configured the device.
    * `{:uvc, uvc, {:streaming, stream}}` - an app on the host opened the
      camera. `stream` is `%{format: :mjpeg | :yuyv, width: w, height: h,
      fps: fps}`; send frames of it with `frame/2`.
    * `{:uvc, uvc, :stopped}` - the app closed it.
    * `{:uvc, uvc, :disconnected}` - the host went away.

  A frame is one JPEG for `:mjpeg`, and `width * height * 2` bytes of YUYV
  (4:2:2, Y0 U Y1 V) for `:yuyv`. Frames sent while nothing streams, or
  faster than the host takes them, are dropped.

  Requirements: `usb_f_uvc` (`CONFIG_USB_CONFIGFS_F_UVC`), the gadget bound
  to a UDC, and the device node readable and writable (root).
  """

  use GenServer

  require Logger

  @typedoc "What a host can be sent."
  @type format :: :mjpeg | :yuyv

  @typedoc "A frame size, with its frame rates or the function's (`:fps`)."
  @type size :: {pos_integer(), pos_integer()} | {pos_integer(), pos_integer(), [pos_integer()]}

  @typedoc "What the host picked."
  @type stream :: %{
          format: format(),
          width: pos_integer(),
          height: pos_integer(),
          fps: pos_integer()
        }

  @default_fps [30, 15]
  @default_max_packet 3072

  @fourcc %{"MJPG" => :mjpeg, "YUYV" => :yuyv}

  @doc """
  The attributes of a `uvc.NAME` function (see `BodgeUSBGadget.define/3`)
  that sends `formats`: each format with its frame sizes, offered to the
  host in this order, the first size being its default.

  ## Options

    * `:fps` - the frame rates of sizes that don't give their own (default
      `[30, 15]`).
    * `:name` - the camera's name, as hosts may show it (default
      `"UVC Camera"`).
    * `:max_packet` - bytes the isochronous endpoint carries per
      microframe: up to 1024, or 2048 or 3072 as two or three packets
      (default 3072, about 24 MB/s at high speed). Uncompressed video
      needs this much: 640x480 YUYV at 30 fps is 18 MB/s.

  The camera offers no controls (brightness and the like).
  """
  @spec function([{format(), [size()]}], keyword()) :: BodgeUSBGadget.attrs()
  def function(formats, opts \\ []) do
    fps = Keyword.get(opts, :fps, @default_fps)
    header = for {format, _sizes} <- formats, do: {dir(format), {:link, format_path(format)}}

    [
      function_name: Keyword.get(opts, :name, "UVC Camera"),
      streaming_maxpacket: Keyword.get(opts, :max_packet, @default_max_packet),
      streaming_interval: 1,
      control: [
        header: [h: []],
        terminal: [camera: [default: [bmControls: "0\n0\n0\n"]]],
        processing: [default: [bmControls: "0\n0\n"]],
        class: [fs: [h: {:link, "control/header/h"}], ss: [h: {:link, "control/header/h"}]]
      ],
      streaming:
        Enum.map(formats, fn {format, sizes} -> format_tree(format, sizes, fps) end) ++
          [
            header: [h: header],
            class: for(speed <- [:fs, :hs, :ss], do: {speed, [h: {:link, "streaming/header/h"}]})
          ]
    ]
  end

  defp format_tree(:mjpeg, sizes, fps), do: {:mjpeg, [m: frames(sizes, fps, 8)]}

  defp format_tree(:yuyv, sizes, fps),
    do: {:uncompressed, [u: [bBitsPerPixel: 16] ++ frames(sizes, fps, 16)]}

  defp dir(:mjpeg), do: :m
  defp dir(:yuyv), do: :u

  defp format_path(:mjpeg), do: "streaming/mjpeg/m"
  defp format_path(:yuyv), do: "streaming/uncompressed/u"

  # A JPEG is taken to be at most a byte a pixel, YUYV is two.
  defp frames(sizes, default_fps, bits) do
    for size <- sizes do
      {w, h, fps} = with {w, h} <- size, do: {w, h, default_fps}
      fps = Enum.sort(fps, :desc)
      intervals = Enum.map(fps, &div(10_000_000, &1))
      bitrate = &(w * h * bits * &1)

      {"#{w}x#{h}",
       [
         wWidth: w,
         wHeight: h,
         dwMinBitRate: bitrate.(List.last(fps)),
         dwMaxBitRate: bitrate.(hd(fps)),
         dwMaxVideoFrameBufferSize: div(w * h * bits, 8),
         dwDefaultFrameInterval: hd(intervals),
         dwFrameInterval: Enum.map_join(intervals, "", &"#{&1}\n")
       ]}
    end
  end

  @doc """
  Start serving the webcam at `opts[:device]` (from
  `BodgeUSBGadget.device_node/2`). `opts[:max_packet]` must be the
  function's (default 3072); `opts[:notify]` (default: the caller) gets the
  events in the moduledoc; `opts[:name]` optionally names the server.
  """
  @spec start_link(keyword()) :: GenServer.on_start()
  def start_link(opts) do
    opts = Keyword.put_new(opts, :notify, self())
    GenServer.start_link(__MODULE__, opts, Keyword.take(opts, [:name]))
  end

  @spec stop(GenServer.server()) :: :ok
  def stop(server), do: GenServer.stop(server)

  @doc "Send the host a frame of the stream it picked."
  @spec frame(GenServer.server(), iodata()) :: :ok
  def frame(server, frame), do: GenServer.cast(server, {:frame, frame})

  @impl GenServer
  def init(opts) do
    device = Keyword.fetch!(opts, :device)
    max_packet = Keyword.get(opts, :max_packet, @default_max_packet)

    port =
      Port.open({:spawn_executable, helper()}, [
        :binary,
        :exit_status,
        packet: 4,
        args: [device, Integer.to_string(max_packet)]
      ])

    {:ok, %{port: port, notify: Keyword.fetch!(opts, :notify)}}
  end

  @impl GenServer
  def handle_cast({:frame, frame}, state) do
    Port.command(state.port, frame)
    {:noreply, state}
  end

  @impl GenServer
  def handle_info({port, {:data, line}}, %{port: port} = state) do
    case String.split(line, " ", parts: 2) do
      ["connect", _speed] -> notify(state, :connected)
      ["disconnect"] -> notify(state, :disconnected)
      ["stop"] -> notify(state, :stopped)
      ["stream", stream] -> notify(state, {:streaming, parse_stream(stream)})
      ["log", text] -> Logger.debug("BodgeUSBGadget.UVC: #{text}")
      ["error", text] -> Logger.warning("BodgeUSBGadget.UVC: #{text}")
      _ -> Logger.warning("BodgeUSBGadget.UVC: unexpected #{inspect(line)}")
    end

    {:noreply, state}
  end

  def handle_info({port, {:exit_status, status}}, %{port: port} = state),
    do: {:stop, {:uvc_gadget_exited, status}, state}

  def handle_info(_message, state), do: {:noreply, state}

  defp notify(state, event), do: send(state.notify, {:uvc, self(), event})

  # "MJPG 1280 720 333333": the interval is in 100 ns units.
  defp parse_stream(stream) do
    [fourcc, w, h, interval] = String.split(stream, " ")
    interval = String.to_integer(interval)

    %{
      format: Map.fetch!(@fourcc, fourcc),
      width: String.to_integer(w),
      height: String.to_integer(h),
      fps: round(10_000_000 / interval)
    }
  end

  defp helper(), do: Path.join(:code.priv_dir(:bodge_usb_gadget), "uvc_gadget")
end
