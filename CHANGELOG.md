# Changelog

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
