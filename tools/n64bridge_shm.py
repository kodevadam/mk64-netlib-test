#!/usr/bin/env python3
"""
n64bridge_shm.py — SC64 Shared Memory bridge for N64 netplay.

Uses SC64's 8KB BlockRAM (mapped at 0x1FFE0000 on N64 PI bus,
0x05000000 internally) as a shared memory region. The N64 game
reads/writes via PI. This bridge reads/writes via SC64 USB commands.

No PI bus contention — the SC64 firmware manages access.

Usage:
    # Upload ROM first:
    sc64deployer upload build/us/mk64.us.z64
    # Reset console, then start bridge:
    sudo ~/n64bridge-venv/bin/python tools/n64bridge_shm.py -s 127.0.0.1:6464

Architecture:
    N64 <--PI (0x1FFE1F80)--> SC64 BlockRAM <--USB cmd--> Bridge <--UDP--> Server

Requirements:
    pip install pyftdi
"""

import sys
import time
import socket
import struct
import argparse

try:
    from pyftdi.ftdi import Ftdi
except ImportError:
    print("Error: pyftdi not installed. Run: pip install pyftdi")
    sys.exit(1)


# ── SC64 USB command protocol ──────────────────────────────────
# Frame: b"CMD" + cmd_id(1) + arg0(BE32) + arg1(BE32) [+ data]
# Response: b"CMP" + cmd_id(1) + data0(BE32) + data1(BE32) [+ data]
# Error:    b"ERR" + cmd_id(1)

SC64_CMD_MEMORY_READ  = ord('m')  # args: address, length → response + data
SC64_CMD_MEMORY_WRITE = ord('M')  # args: address, length + data → response

# SC64 internal address for the BlockRAM that maps to N64 PI 0x1FFE0000
SC64_BRAM_BASE = 0x05000000

# ── Netplay SHM register map ──────────────────────────────────
# N64 PI address: 0x1FFE1F80 + offset
# SC64 internal:  0x05001F80 + offset
NP_N64_BASE  = 0x1FFE1F80
NP_SC64_BASE = SC64_BRAM_BASE + (NP_N64_BASE - 0x1FFE0000)  # 0x05001F80

NP_MAGIC_VAL = 0x4E455450  # "NETP"

# Register offsets
REG_MAGIC        = 0x00
REG_ENABLE       = 0x04
REG_STATUS       = 0x08
REG_FRAME        = 0x0C
REG_OVERRIDE     = 0x10  # +i*4 for player i (remote inputs written by bridge)
REG_LOCAL        = 0x20  # +i*4 for player i (local inputs written by N64)
REG_CTRL_MASK    = 0x30
REG_INPUT_DELAY  = 0x34
REG_LOCAL_PLAYER = 0x38
REG_RNG_SEED     = 0x3C
REG_PLAYER_COUNT = 0x40
REG_FRAME_RDY    = 0x44
REG_GAME_MODE    = 0x48
REG_COURSE_ID    = 0x4C
REG_CHAR_SEL     = 0x50  # +i*4
REG_RACE_START   = 0x60
REG_DISCONNECT   = 0x64
REG_CC_SELECT    = 0x68

STATUS_N64_READY = 0x01
STATUS_IN_RACE   = 0x02


class SC64:
    """SC64 USB command interface via pyftdi."""

    def __init__(self, device_index=0):
        self.ftdi = Ftdi()
        self.index = device_index

    def open(self):
        devices = Ftdi.list_devices()
        if not devices:
            raise RuntimeError("No FTDI devices found")
        if self.index >= len(devices):
            raise RuntimeError(f"Device {self.index} not found ({len(devices)} available)")

        desc, _ = devices[self.index]
        url = f"ftdi://0x{desc.vid:04x}:0x{desc.pid:04x}/{self.index + 1}"
        self.ftdi.open_from_url(url)
        self.ftdi.set_baudrate(115200)
        self.ftdi.set_latency_timer(2)
        self.ftdi.purge_buffers()
        print(f"SC64 opened (SN={desc.sn or 'N/A'})")

    def close(self):
        try:
            self.ftdi.close()
        except Exception:
            pass

    def _tx(self, data):
        self.ftdi.write_data(data)

    def _rx(self, n, timeout=2.0):
        buf = bytearray()
        deadline = time.monotonic() + timeout
        while len(buf) < n and time.monotonic() < deadline:
            chunk = self.ftdi.read_data(n - len(buf))
            if chunk:
                buf.extend(chunk)
            else:
                time.sleep(0.0005)
        return bytes(buf)

    def _cmd(self, cmd_id, arg0=0, arg1=0, tx_data=b'', rx_data_len=0):
        """Execute an SC64 command and return (data0, data1, rx_data)."""
        frame = b'CMD' + struct.pack(">BIII", cmd_id, arg0, arg1, len(tx_data))
        self._tx(frame + tx_data)

        # Read response: "CMP" + cmd_id(1) + data0(4) + data1(4) = 12 bytes
        resp = self._rx(12 + rx_data_len, timeout=2.0)
        if len(resp) < 4:
            raise RuntimeError(f"No response for cmd 0x{cmd_id:02x}")
        if resp[0:3] == b'ERR':
            raise RuntimeError(f"SC64 error for cmd 0x{cmd_id:02x}")
        if resp[0:3] != b'CMP':
            raise RuntimeError(f"Unexpected response: {resp[0:3]}")

        data0 = struct.unpack(">I", resp[4:8])[0] if len(resp) >= 8 else 0
        data1 = struct.unpack(">I", resp[8:12])[0] if len(resp) >= 12 else 0
        rx_data = resp[12:] if len(resp) > 12 else b''
        return data0, data1, rx_data

    def mem_read(self, addr, length=4):
        """Read bytes from SC64 memory space."""
        _, _, data = self._cmd(SC64_CMD_MEMORY_READ, addr, length, rx_data_len=length)
        return data

    def mem_write(self, addr, data):
        """Write bytes to SC64 memory space."""
        self._cmd(SC64_CMD_MEMORY_WRITE, addr, len(data), tx_data=data)

    def read32(self, addr):
        """Read a 32-bit BE word."""
        data = self.mem_read(addr, 4)
        if len(data) < 4:
            return None
        return struct.unpack(">I", data)[0]

    def write32(self, addr, val):
        """Write a 32-bit BE word."""
        self.mem_write(addr, struct.pack(">I", val))

    # ── SHM convenience methods ──

    def shm_read(self, offset):
        return self.read32(NP_SC64_BASE + offset)

    def shm_write(self, offset, val):
        self.write32(NP_SC64_BASE + offset, val)


class Bridge:
    """Shared-memory bridge: SC64 BlockRAM ↔ UDP server."""

    def __init__(self, sc64, server_host, server_port,
                 local_player=0, player_count=2):
        self.sc64 = sc64
        self.server = (server_host, server_port)
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setblocking(False)
        self.local = local_player
        self.count = player_count
        self.prev_frame = 0

    def setup_shm(self):
        """Write initial netplay config into SHM before N64 boots."""
        seed = int(time.time()) & 0xFFFFFFFF
        print(f"Setting up SHM: player={self.local}, count={self.count}, seed=0x{seed:08X}")

        self.sc64.shm_write(REG_MAGIC, NP_MAGIC_VAL)
        self.sc64.shm_write(REG_ENABLE, 1)
        self.sc64.shm_write(REG_LOCAL_PLAYER, self.local)
        self.sc64.shm_write(REG_PLAYER_COUNT, self.count)
        self.sc64.shm_write(REG_CTRL_MASK, (1 << self.count) - 1)
        self.sc64.shm_write(REG_INPUT_DELAY, 2)
        self.sc64.shm_write(REG_RNG_SEED, seed)
        self.sc64.shm_write(REG_STATUS, 0)
        self.sc64.shm_write(REG_DISCONNECT, 0)
        self.sc64.shm_write(REG_RACE_START, 0)
        self.sc64.shm_write(REG_FRAME_RDY, 0)

        # Zero override and local input registers
        for i in range(4):
            self.sc64.shm_write(REG_OVERRIDE + i * 4, 0)
            self.sc64.shm_write(REG_LOCAL + i * 4, 0)

        print("SHM configured. Reset the N64 to activate netplay.")

    def teardown_shm(self):
        """Clear SHM so game boots normally next time."""
        self.sc64.shm_write(REG_MAGIC, 0)
        self.sc64.shm_write(REG_ENABLE, 0)
        print("SHM cleared.")

    def run(self):
        """Main bridge loop."""
        print(f"Bridge running → {self.server[0]}:{self.server[1]}")
        print("Press Ctrl+C to stop.\n")

        polls = 0
        errors = 0

        try:
            while True:
                try:
                    frame = self.sc64.shm_read(REG_FRAME)
                except Exception as e:
                    errors += 1
                    if errors > 10:
                        print(f"Too many read errors: {e}")
                        break
                    time.sleep(0.05)
                    continue

                if frame is None:
                    time.sleep(0.01)
                    continue

                errors = 0

                if frame != self.prev_frame:
                    self.prev_frame = frame
                    self._on_frame(frame)
                    polls += 1

                    if polls % 300 == 0:
                        status = self.sc64.shm_read(REG_STATUS) or 0
                        state = "RACING" if (status & STATUS_IN_RACE) else "menu"
                        ready = "ready" if (status & STATUS_N64_READY) else "init"
                        print(f"  [frame {frame}] {state} | {ready}")
                else:
                    # Drain UDP even between frames
                    self._recv_remote()
                    time.sleep(0.001)

        except KeyboardInterrupt:
            print("\nStopping...")
        finally:
            self.teardown_shm()
            self.sock.close()

    def _on_frame(self, frame):
        """Called each time the N64 advances a frame."""
        # 1. Read local input from N64
        local_input = self.sc64.shm_read(REG_LOCAL + self.local * 4)
        if local_input is None:
            return

        # 2. Send to server: [player:1][frame:4][input:4]
        pkt = struct.pack(">BII", self.local, frame, local_input)
        try:
            self.sock.sendto(pkt, self.server)
        except Exception:
            pass

        # 3. Write incoming remote inputs
        self._recv_remote()

        # 4. Signal frame ready to N64
        self.sc64.shm_write(REG_FRAME_RDY, frame)

    def _recv_remote(self):
        """Read all pending UDP packets and write remote inputs to SHM."""
        try:
            while True:
                data, _ = self.sock.recvfrom(4096)
                if len(data) >= 9:
                    player, frame, inp = struct.unpack(">BII", data[:9])
                    if player != self.local and player < 8:
                        self.sc64.shm_write(REG_OVERRIDE + player * 4, inp)
        except BlockingIOError:
            pass


def main():
    p = argparse.ArgumentParser(description="SC64 SHM netplay bridge")
    p.add_argument("--server", "-s", default="127.0.0.1:6464")
    p.add_argument("--device", "-d", type=int, default=0)
    p.add_argument("--player", "-p", type=int, default=0)
    p.add_argument("--players", "-n", type=int, default=2)
    p.add_argument("--list", "-l", action="store_true")
    p.add_argument("--setup-only", action="store_true",
                   help="Write SHM config and exit (for testing)")
    args = p.parse_args()

    if args.list:
        for i, (desc, _) in enumerate(Ftdi.list_devices()):
            print(f"  [{i}] VID=0x{desc.vid:04x} PID=0x{desc.pid:04x} SN={desc.sn or 'N/A'}")
        return

    host, port = args.server.rsplit(":", 1)

    sc64 = SC64(args.device)
    sc64.open()

    bridge = Bridge(sc64, host, int(port), args.player, args.players)
    bridge.setup_shm()

    if args.setup_only:
        print("SHM written. Exiting (--setup-only).")
        sc64.close()
        return

    try:
        bridge.run()
    finally:
        sc64.close()


if __name__ == "__main__":
    main()
