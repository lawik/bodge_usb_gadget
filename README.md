# BodgeUSBGadget

Be a USB device from Elixir on Linux. The device-side companion to
[`bodge_usb`](https://github.com/lawik/bodge_usb) (host-side USB): where that
library talks *to* USB devices, this one makes the machine *be* one, on
UDC-capable hardware (OTG ports on Pi Zero/4, BeagleBone, most Nerves
targets, or `dummy_hcd` for a virtual UDC).

Two capabilities:

- `BodgeUSBGadget`: define kernel-implemented device classes (HID, serial,
  ethernet, mass storage, ...) as a configfs gadget tree; bind/unbind to a
  UDC; resolve the resulting `/dev` nodes and network interfaces. Pure
  filesystem plumbing, no processes.
- `BodgeUSBGadget.FunctionFs`: serve fully custom device functions from
  Elixir over FunctionFS. Vendor control requests arrive as handler
  callbacks; endpoints are driven with blocking `read/2`/`write/2` (dirty
  I/O schedulers, no blocked BEAM schedulers).

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
descriptors, SETUP handler, endpoint files).

## Requirements

Linux with configfs mounted, `libcomposite` and the `usb_f_*` modules for the
functions used, a UDC, and root (or equivalent permissions on
`/sys/kernel/config`).

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
