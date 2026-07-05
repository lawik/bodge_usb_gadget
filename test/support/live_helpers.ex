# SPDX-FileCopyrightText: 2026 Lars Wikman
#
# SPDX-License-Identifier: Apache-2.0

defmodule BodgeUSBGadget.LiveHelpers do
  @moduledoc false

  # Helpers for the device-backed (VM) tests: poll the host side of the
  # library pair until the gadget under test appears on / leaves the bus.

  @spec await_device(0..0xFFFF, 0..0xFFFF, non_neg_integer()) :: struct() | nil
  def await_device(_vid, _pid, 0), do: nil

  def await_device(vid, pid, tries) do
    case BodgeUSB.find_device(vid, pid) do
      nil ->
        Process.sleep(100)
        await_device(vid, pid, tries - 1)

      ref ->
        ref
    end
  end

  @spec await_gone(0..0xFFFF, 0..0xFFFF, non_neg_integer()) :: :ok | :still_present
  def await_gone(_vid, _pid, 0), do: :still_present

  def await_gone(vid, pid, tries) do
    case BodgeUSB.find_device(vid, pid) do
      nil ->
        :ok

      _ref ->
        Process.sleep(100)
        await_gone(vid, pid, tries - 1)
    end
  end
end
