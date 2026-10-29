# SPDX-FileCopyrightText: 2026 Lars Wikman
#
# SPDX-License-Identifier: Apache-2.0

defmodule BodgeUSBGadget.UVCTest do
  use ExUnit.Case, async: true

  alias BodgeUSBGadget, as: Gadget
  alias BodgeUSBGadget.UVC

  setup do
    root = Path.join(System.tmp_dir!(), "uvc_test_#{System.unique_integer([:positive])}")
    File.mkdir_p!(root)
    on_exit(fn -> File.rm_rf!(root) end)
    %{root: root}
  end

  defp define(root, function) do
    Gadget.define(
      "cam",
      %{functions: %{"uvc.cam" => function}, configs: %{"c.1" => %{functions: ["uvc.cam"]}}},
      root: root
    )
  end

  test "function/2 lays out formats, frames, headers and class links", %{root: root} do
    formats = [mjpeg: [{1280, 720}, {640, 480, [30]}], yuyv: [{640, 480}]]
    {:ok, g} = define(root, UVC.function(formats, name: "Phone camera"))
    f = Path.join(g.path, "functions/uvc.cam")
    read = &File.read!(Path.join(f, &1))

    assert read.("function_name") == "Phone camera"
    assert read.("streaming_maxpacket") == "3072"

    m = "streaming/mjpeg/m/1280x720/"
    assert read.(m <> "wWidth") == "1280"
    assert read.(m <> "wHeight") == "720"
    # Fastest first, in 100 ns units.
    assert read.(m <> "dwFrameInterval") == "333333\n666666\n"
    assert read.(m <> "dwDefaultFrameInterval") == "333333"
    assert read.(m <> "dwMaxVideoFrameBufferSize") == "921600"
    assert read.("streaming/mjpeg/m/640x480/dwFrameInterval") == "333333\n"

    u = "streaming/uncompressed/u/"
    assert read.(u <> "bBitsPerPixel") == "16"
    assert read.(u <> "640x480/dwMaxVideoFrameBufferSize") == "614400"

    # No controls.
    assert read.("control/terminal/camera/default/bmControls") == "0\n0\n0\n"
    assert read.("control/processing/default/bmControls") == "0\n0\n"

    links = %{
      "streaming/header/h/m" => "streaming/mjpeg/m",
      "streaming/header/h/u" => "streaming/uncompressed/u",
      "streaming/class/fs/h" => "streaming/header/h",
      "streaming/class/hs/h" => "streaming/header/h",
      "streaming/class/ss/h" => "streaming/header/h",
      "control/class/fs/h" => "control/header/h",
      "control/class/ss/h" => "control/header/h"
    }

    for {link, target} <- links do
      assert File.read_link!(Path.join(f, link)) == Path.join(f, target)
    end
  end

  test "function/2 takes the frame rates and packet size", %{root: root} do
    {:ok, g} = define(root, UVC.function([yuyv: [{640, 360}]], fps: [10, 30], max_packet: 1024))
    f = Path.join(g.path, "functions/uvc.cam")

    assert File.read!(Path.join(f, "streaming_maxpacket")) == "1024"

    assert File.read!(Path.join(f, "streaming/uncompressed/u/640x360/dwFrameInterval")) ==
             "333333\n1000000\n"

    refute File.exists?(Path.join(f, "streaming/header/h/m"))
  end
end
