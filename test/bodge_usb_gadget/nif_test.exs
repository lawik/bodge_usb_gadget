# SPDX-FileCopyrightText: 2026 Lars Wikman
#
# SPDX-License-Identifier: Apache-2.0

defmodule BodgeUSBGadget.NifTest do
  use ExUnit.Case, async: false

  alias BodgeUSBGadget.Nif

  describe "open/close/read/write on device files" do
    test "reads zeros from /dev/zero" do
      assert {:ok, h} = Nif.open("/dev/zero", [:rdonly])
      assert is_integer(Nif.fileno(h))
      assert {:ok, <<0, 0, 0, 0>>} = Nif.read(h, 4)
      assert :ok = Nif.close(h)
    end

    test "writes to /dev/null and reports the byte count" do
      assert {:ok, h} = Nif.open("/dev/null", [:wronly])
      assert {:ok, 5} = Nif.write(h, "hello")
      assert {:ok, 3} = Nif.write(h, [?a, "b", ?c])
      assert :ok = Nif.close(h)
    end

    test "read_blocking/write_blocking mirror read/write on ordinary fds" do
      {:ok, z} = Nif.open("/dev/zero", [:rdonly])
      assert {:ok, <<0, 0, 0, 0>>} = Nif.read_blocking(z, 4)
      assert :ok = Nif.close(z)

      {:ok, null} = Nif.open("/dev/null", [:wronly])
      assert {:ok, 5} = Nif.write_blocking(null, "hello")
      assert :ok = Nif.close(null)
    end

    test "close is idempotent; I/O on a closed handle is :ebadf" do
      {:ok, h} = Nif.open("/dev/zero", [:rdonly])
      assert :ok = Nif.close(h)
      assert :ok = Nif.close(h)
      assert {:error, :ebadf} = Nif.read(h, 4)
      assert {:error, :ebadf} = Nif.read_blocking(h, 4)
      assert {:error, :ebadf} = Nif.write_blocking(h, "x")
      assert {:error, :ebadf} = Nif.fileno(h)
    end

    test "missing path -> :enoent; bad args raise ArgumentError" do
      assert {:error, :enoent} = Nif.open("/no/such/ffs/ep0", [:rdwr])
      assert_raise ArgumentError, fn -> Nif.open("/dev/zero", [:bogus]) end
      assert_raise ArgumentError, fn -> Nif.read(make_ref(), 4) end
    end
  end

  describe "resource lifecycle" do
    test "dropped handles are closed on GC (no fd leak)" do
      fdcount = fn -> length(File.ls!("/proc/self/fd")) end
      before = fdcount.()

      # Open in rounds, dropping each handle without closing, and force GC
      # after each round: the destructor must reclaim dropped fds without the
      # live count ever exceeding the open-file ulimit.
      for _round <- 1..20 do
        Enum.each(1..200, fn _ -> {:ok, _h} = Nif.open("/dev/null", [:rdonly]) end)
        :erlang.garbage_collect()
      end

      Process.sleep(50)
      leaked = fdcount.() - before
      assert leaked < 50, "leaked #{leaked} fds across 4000 open+drop+GC"
    end
  end
end
