# Changelog

## Unreleased

- Webcams (`BodgeUSBGadget.UVC`): `function/2` builds a UVC function's
  configfs tree for the MJPEG and YUYV sizes and frame rates offered; the
  server runs a helper program (`priv/uvc_gadget`) that answers the host's
  probe/commit negotiation from what the device enumerates and queues the
  frames it is sent, and reports when a host connects, starts and stops
  streaming. `device_node/2` resolves a `uvc` function's V4L2 device.
- Nested functions: in a function's attributes, a map or `{name, value}`
  list is a subdirectory and `{:link, path}` a symlink within the function,
  made once everything else is in place. `remove/1` takes such trees down
  in the order configfs needs.

## v0.1.0

Initial release, split out of the host-side library (now `bodge_usb`) so the
device side can evolve on its own schedule.

- Device-side gadgets over configfs (`BodgeUSBGadget`): declarative
  define/bind/unbind/remove, device-node and network-interface resolution
  for kernel function drivers (HID, ACM, ECM, ...).
- Custom device functions over FunctionFS (`BodgeUSBGadget.FunctionFs`):
  descriptor/strings blob construction, ep0 event loop with a SETUP handler
  contract, endpoint files via blocking `read/2`/`write/2` (dirty I/O with
  the fd lock released across the syscall).
- Own minimal fd-shim NIF (open/close/read/write, blocking variants,
  read-readiness select); no runtime dependency on `bodge_usb`.
