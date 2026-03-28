#!/usr/bin/env python3
"""
n64bridge_shm.py — SC64 Shared Memory bridge for N64 netplay.

Uses SC64's memory-mapped registers instead of USB packet streaming.
The N64 game reads/writes shared memory at 0x1FFE1F80 via PI bus.
This bridge reads/writes the same memory via SC64 USB commands.

No PI bus contention since SC64 firmware manages the access.

Usage:
    sudo ~/n64bridge-venv/bin/python tools/n64bridge_shm.py --server 127.0.0.1:6464

Architecture:
    N64 <--PI--> SC64 SHM registers <--USB--> This bridge <--UDP--> Java server

Requirements:
    pip install pyftdi
"""

import sys
import time
import socket
import struct
import argparse
import threading

try:
    from pyftdi.ftdi import Ftdi
except ImportError:
    print("Error: pyftdi not installed. Run: pip install pyftdi")
    sys.exit(1)


# SC64 register addresses (directly accessible via PI from N64 side)
SC64_REGS_BASE   = 0x1FFF0000
SC64_REG_SR_CMD  = SC64_REGS_BASE + 0x00
SC64_REG_DATA_0  = SC64_REGS_BASE + 0x04
SC64_REG_DATA_1  = SC64_REGS_BASE + 0x08
SC64_REG_KEY     = SC64_REGS_BASE + 0x10

SC64_KEY_RESET   = 0x00000000
SC64_KEY_UNLOCK_1 = 0x5F554E4C
SC64_KEY_UNLOCK_2 = 0x4F434B5F

SC64_SR_CMD_BUSY  = (1 << 31)
SC64_SR_CMD_ERROR = (1 << 30)

SC64_CMD_CONFIG_SET = ord('C')
SC64_CFG_ROM_WRITE_ENABLE = 1

# Netplay shared memory layout at NP_SHM_BASE
NP_SHM_BASE      = 0x1FFE1F80
NP_MAGIC          = 0x4E455450  # "NETP"

# Register offsets from NP_SHM_BASE
NP_REG_MAGIC        = 0x00
NP_REG_ENABLE       = 0x04
NP_REG_STATUS       = 0x08
NP_REG_FRAME        = 0x0C
NP_REG_OVERRIDE_0   = 0x10  # 4 u32 slots (0x10..0x1C)
NP_REG_LOCAL_0       = 0x20  # 4 u32 slots (0x20..0x2C)
NP_REG_CTRL_MASK    = 0x30
NP_REG_INPUT_DELAY  = 0x34
NP_REG_LOCAL_PLAYER = 0x38
NP_REG_RNG_SEED     = 0x3C
NP_REG_PLAYER_COUNT = 0x40
NP_REG_MCU_FRAME_RDY = 0x44
NP_REG_GAME_MODE    = 0x48
NP_REG_COURSE_ID    = 0x4C
NP_REG_CHAR_SEL_0   = 0x50  # 4 u32 slots (0x50..0x5C)
NP_REG_RACE_START   = 0x60
NP_REG_DISCONNECT   = 0x64
NP_REG_CC_SELECT    = 0x68

# N64 status flags
NP_STATUS_N64_READY = 0x01
NP_STATUS_IN_RACE   = 0x02

# SC64 SDRAM base for ROM
SC64_BASE = 0x10000000


class SC64Device:
    """Direct SC64 hardware access via FTDI USB."""

    def __init__(self, device_index=0):
        self.ftdi = Ftdi()
        self.device_index = device_index

    def open(self):
        devices = Ftdi.list_devices()
        if not devices:
            raise RuntimeError("No FTDI devices found")

        if self.device_index >= len(devices):
            raise RuntimeError(f"Device index {self.device_index} out of range")

        desc, _ = devices[self.device_index]
        url = f"ftdi://0x{desc.vid:04x}:0x{desc.pid:04x}/{self.device_index + 1}"
        self.ftdi.open_from_url(url)
        self.ftdi.set_baudrate(115200)
        self.ftdi.set_latency_timer(2)
        self.ftdi.purge_buffers()
        print(f"SC64 connected (SN={desc.sn or 'N/A'})")

    def close(self):
        try:
            self.ftdi.close()
        except Exception:
            pass

    def _write_bytes(self, data):
        self.ftdi.write_data(data)

    def _read_bytes(self, count, timeout=2.0):
        buf = bytearray()
        deadline = time.monotonic() + timeout
        while len(buf) < count and time.monotonic() < deadline:
            chunk = self.ftdi.read_data(count - len(buf))
            if chunk:
                buf.extend(chunk)
            else:
                time.sleep(0.001)
        return bytes(buf)

    def pi_read(self, address):
        """Read a 32-bit value from an N64 PI address via SC64."""
        # SC64 USB protocol: send command to read PI register
        # We use the SC64's register interface
        # Write the address to DATA_0, execute a read command
        #
        # For simplicity, we use sc64deployer's protocol:
        # The SC64 USB protocol uses a simple command frame:
        #   'C' 'M' 'D' cmd_id [data_0:4] [data_1:4] -> response [data_0:4] [data_1:4]
        cmd = b'CMD' + struct.pack(">B", ord('v'))  # 'v' = VERSION/identify
        # Actually, let's use the memory read approach through SDRAM
        # SC64 maps PI addresses, so we can read SDRAM at the SHM offset

        # The proper approach: use SC64 firmware's debug protocol
        # For now, let's use a simpler approach via the deployer
        pass

    def memory_read_32(self, pi_address):
        """Read a 32-bit word from SC64 address space.

        Uses the SC64's register interface: write address to DATA_0,
        send read command, read result from DATA_0.
        """
        # SC64 USB frame format (from sc64 firmware source):
        # TX: 'C' 'M' 'D' cmd_id [data_0 BE32] [data_1 BE32]
        # RX: 'C' 'M' 'P' cmd_id [data_0 BE32] [data_1 BE32]
        # or: 'E' 'R' 'R' cmd_id
        #
        # We'll use cmd 'm' (MEMORY_READ) with:
        #   data_0 = address
        #   data_1 = length (4 bytes)
        # Response includes the data after the CMP header

        frame = b'CMD' + struct.pack(">B II", ord('m'), pi_address, 4)
        self._write_bytes(frame)

        # Read response header (4 bytes) + data_0 (4) + data_1 (4) + actual data (4)
        resp = self._read_bytes(16, timeout=1.0)
        if len(resp) < 16:
            return None
        if resp[0:3] == b'CMP':
            # Data follows after the 12-byte header
            return struct.unpack(">I", resp[12:16])[0]
        elif resp[0:3] == b'ERR':
            return None
        return None

    def memory_write_32(self, pi_address, value):
        """Write a 32-bit word to SC64 address space."""
        # cmd 'M' (MEMORY_WRITE) with:
        #   data_0 = address
        #   data_1 = length (4 bytes)
        # Then send the actual data
        frame = b'CMD' + struct.pack(">B II", ord('M'), pi_address, 4)
        frame += struct.pack(">I", value)
        self._write_bytes(frame)

        # Read response
        resp = self._read_bytes(12, timeout=1.0)
        if len(resp) >= 3 and resp[0:3] == b'CMP':
            return True
        return False


class SHMBridge:
    """Shared Memory bridge between SC64 and UDP server."""

    def __init__(self, server_host, server_port, device_index=0, local_player=0, player_count=2):
        self.server_host = server_host
        self.server_port = server_port
        self.device = SC64Device(device_index)
        self.udp_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.udp_socket.setblocking(False)
        self.running = False
        self.local_player = local_player
        self.player_count = player_count
        self.last_n64_frame = 0

    def start(self):
        print(f"SHM Bridge — Player {self.local_player}, {self.player_count} total")
        print(f"Server: {self.server_host}:{self.server_port}")

        self.device.open()

        # Write the magic value to signal netplay is active
        print("Writing netplay config to SHM...")
        self.device.memory_write_32(NP_SHM_BASE + NP_REG_MAGIC, NP_MAGIC)
        self.device.memory_write_32(NP_SHM_BASE + NP_REG_ENABLE, 1)
        self.device.memory_write_32(NP_SHM_BASE + NP_REG_LOCAL_PLAYER, self.local_player)
        self.device.memory_write_32(NP_SHM_BASE + NP_REG_PLAYER_COUNT, self.player_count)
        self.device.memory_write_32(NP_SHM_BASE + NP_REG_CTRL_MASK, (1 << self.player_count) - 1)
        self.device.memory_write_32(NP_SHM_BASE + NP_REG_INPUT_DELAY, 2)
        self.device.memory_write_32(NP_SHM_BASE + NP_REG_RNG_SEED, int(time.time()) & 0xFFFFFFFF)
        self.device.memory_write_32(NP_SHM_BASE + NP_REG_STATUS, 0)
        self.device.memory_write_32(NP_SHM_BASE + NP_REG_DISCONNECT, 0)
        self.device.memory_write_32(NP_SHM_BASE + NP_REG_RACE_START, 0)
        print("SHM config written. N64 should detect netplay on next boot/reset.")

        self.running = True

        try:
            print("Bridge running. Press Ctrl+C to stop.")
            self._poll_loop()
        except KeyboardInterrupt:
            print("\nStopping...")
        finally:
            # Clear magic to disable netplay
            self.device.memory_write_32(NP_SHM_BASE + NP_REG_MAGIC, 0)
            self.device.memory_write_32(NP_SHM_BASE + NP_REG_ENABLE, 0)
            self.running = False
            self.device.close()
            self.udp_socket.close()

    def _poll_loop(self):
        """Main polling loop — read N64 state, relay inputs, write remote inputs."""
        poll_count = 0

        while self.running:
            # Read N64 status
            status = self.device.memory_read_32(NP_SHM_BASE + NP_REG_STATUS)
            if status is None:
                time.sleep(0.1)
                continue

            # Read N64 frame counter
            frame = self.device.memory_read_32(NP_SHM_BASE + NP_REG_FRAME)
            if frame is None:
                time.sleep(0.01)
                continue

            # Only process when frame advances
            if frame != self.last_n64_frame:
                self.last_n64_frame = frame

                # Read local player's input from N64
                local_input = self.device.memory_read_32(
                    NP_SHM_BASE + NP_REG_LOCAL_0 + (self.local_player * 4)
                )
                if local_input is not None:
                    # Send to server: [player_id:1][frame:4][input:4]
                    pkt = struct.pack(">BII", self.local_player, frame, local_input)
                    try:
                        self.udp_socket.sendto(pkt, (self.server_host, self.server_port))
                    except Exception:
                        pass

                # Check for incoming remote inputs from server
                try:
                    while True:
                        data, _ = self.udp_socket.recvfrom(4096)
                        if len(data) >= 9:
                            remote_player, remote_frame, remote_input = struct.unpack(">BII", data[:9])
                            if remote_player != self.local_player:
                                # Write remote input to N64's override register
                                self.device.memory_write_32(
                                    NP_SHM_BASE + NP_REG_OVERRIDE_0 + (remote_player * 4),
                                    remote_input
                                )
                except BlockingIOError:
                    pass

                if poll_count % 600 == 0:
                    in_race = "RACING" if (status & NP_STATUS_IN_RACE) else "menu"
                    ready = "ready" if (status & NP_STATUS_N64_READY) else "waiting"
                    print(f"[Frame {frame}] {in_race} | {ready}")

                poll_count += 1
            else:
                time.sleep(0.001)  # ~1ms between polls


def main():
    parser = argparse.ArgumentParser(description="SC64 SHM bridge for N64 netplay")
    parser.add_argument("--server", "-s", default="127.0.0.1:6464",
                       help="Server address (default: 127.0.0.1:6464)")
    parser.add_argument("--device", "-d", type=int, default=0,
                       help="FTDI device index (default: 0)")
    parser.add_argument("--player", "-p", type=int, default=0,
                       help="Local player index (default: 0)")
    parser.add_argument("--players", "-n", type=int, default=2,
                       help="Total player count (default: 2)")
    parser.add_argument("--list", "-l", action="store_true",
                       help="List FTDI devices")
    args = parser.parse_args()

    if args.list:
        devices = Ftdi.list_devices()
        if not devices:
            print("No FTDI devices found.")
        else:
            for i, (desc, _) in enumerate(devices):
                print(f"  [{i}] VID=0x{desc.vid:04x} PID=0x{desc.pid:04x} SN={desc.sn or 'N/A'}")
        return

    host, port = args.server.rsplit(":", 1)
    bridge = SHMBridge(host, int(port), args.device, args.player, args.players)
    bridge.start()


if __name__ == "__main__":
    main()
