# BodgeUSBGadget

Be a USB device from Elixir on Linux. The device-side companion to
[`bodge_usb`](https://github.com/lawik/bodge_usb) (host-side USB): where that
library talks *to* USB devices, this one makes the machine *be* one, on
UDC-capable hardware (OTG ports on Pi Zero/4, BeagleBone, most Nerves
targets, or `dummy_hcd` for a virtual UDC).

Three capabilities:

- `BodgeUSBGadget`: define kernel-implemented device classes (HID, serial,
  ethernet, mass storage, webcam, ...) as a configfs gadget tree, nested
  functions included; bind/unbind to a UDC; resolve the resulting `/dev`
  nodes and network interfaces. Pure filesystem plumbing, no processes.
- `BodgeUSBGadget.FunctionFs`: serve fully custom device functions from
  Elixir over FunctionFS. Vendor control requests arrive as handler
  callbacks; endpoints are driven with blocking `read/2`/`write/2` (dirty
  I/O schedulers, no blocked BEAM schedulers).
- `BodgeUSBGadget.UVC`: be a webcam. `UVC.function/2` lays out the kernel's
  UVC function for the formats and sizes you offer (MJPEG, YUYV), and the
  server answers the host's format negotiation and sends the frames you
  give it, through a small helper program (`priv/uvc_gadget`).

## Example

```elixir
spec = %{
  vendor_id: 0xCAFE,
  product_id: 0xBABE,
  strings: %{manufacturer: "bodge", product: "demo", serialnumber: "g-1"},
  functions: %{
    "hid.usb0" => %{protocol: 0, subclass: 0, report_length: 8, report_desc: report_desc}
  },
  configs: %{"c.1" => %{configuration: "demo", max_power: 120, functions: ["hid.usb0"]}}
}

{:ok, g} = BodgeUSBGadget.define("demo", spec)
:ok = BodgeUSBGadget.bind(g)
{:ok, "/dev/hidg0"} = BodgeUSBGadget.device_node(g, "hid.usb0")
```

See `BodgeUSBGadget.FunctionFs` for the custom-function lifecycle (mount,
descriptors, SETUP handler, endpoint files), and `BodgeUSBGadget.UVC` for a
webcam:

```elixir
{:ok, g} =
  BodgeUSBGadget.define("webcam", %{
    vendor_id: 0x1D6B,
    product_id: 0x0102,
    functions: %{"uvc.cam" => BodgeUSBGadget.UVC.function(mjpeg: [{1280, 720}])},
    configs: %{"c.1" => %{configuration: "webcam", functions: ["uvc.cam"]}}
  })

:ok = BodgeUSBGadget.bind(g)
{:ok, device} = BodgeUSBGadget.device_node(g, "uvc.cam")
{:ok, uvc} = BodgeUSBGadget.UVC.start_link(device: device)

# Once the host opens the camera:
#   {:uvc, ^uvc, {:streaming, %{format: :mjpeg, width: 1280, height: 720, fps: 30}}}
BodgeUSBGadget.UVC.frame(uvc, jpeg)
```

## Requirements

Linux with configfs mounted, `libcomposite` and the `usb_f_*` modules for the
functions used (`usb_f_uvc`, `CONFIG_USB_CONFIGFS_F_UVC`, for a webcam), a
UDC, and root (or equivalent permissions on `/sys/kernel/config`).

## Testing

`mix test` runs the host-safe suite anywhere (spec validation, configfs tree
construction against a tmpdir, FunctionFS blob construction). The
device-backed tests (`:usbfs_gadget`, `:usbfs_ffs` tags) exercise both roles
end to end against `dummy_hcd`, with `bodge_usb` as the host driver; run them
inside the bodge_usb VM harness (see `harness/` in that repo) with
`mix test --only usbfs_gadget` and `mix test --only usbfs_ffs` as root.

## Installation

```elixir
def deps do
  [
    {:bodge_usb_gadget, "~> 0.1.0"}
  ]
end
```
