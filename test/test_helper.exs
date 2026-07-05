# SPDX-FileCopyrightText: 2026 Lars Wikman
#
# SPDX-License-Identifier: Apache-2.0

# Device-backed tests are tagged and excluded by default so `mix test` runs the
# host-safe suite anywhere. They need a UDC (dummy_hcd or OTG hardware),
# configfs, the usb_f_* modules, and root; run them inside the bodge_usb VM
# harness with `mix test --only <tag>`:
#   :usbfs_gadget - configfs gadget defined here, driven by bodge_usb host side
#   :usbfs_ffs    - FunctionFS custom function served here, driven by bodge_usb
ExUnit.start(
  exclude: [
    :usbfs_gadget,
    :usbfs_ffs
  ]
)
