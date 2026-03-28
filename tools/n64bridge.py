#!/usr/bin/env python3
"""
n64bridge.py — Minimal USB↔UDP bridge for N64-NetLib netplay.

Connects to an N64 flashcart (SC64, 64Drive, EverDrive) via FTDI USB
and forwards NetLib packets to/from a UDP game server.

Usage:
    python3 n64bridge.py [--server HOST:PORT] [--device INDEX]

Requirements:
    pip install pyftdi

Based on the N64-NetLib protocol (buu342/N64-NetLib).
"""

import sys
import time
import socket
import struct
import argparse
import threading
from collections import deque

try:
    from pyftdi.ftdi import Ftdi
except ImportError:
    print("Error: pyftdi not installed. Run: pip install pyftdi")
    sys.exit(1)


# USB data types (from usb.h)
DATATYPE_TEXT       = 0x01
DATATYPE_RAWBINARY  = 0x02
DATATYPE_HEADER     = 0x03
DATATYPE_SCREENSHOT = 0x04
DATATYPE_HEARTBEAT  = 0x05
DATATYPE_RDBPACKET  = 0x06
DATATYPE_NETPACKET  = 0x27  # NetLib game packets

# UNFLoader protocol
USBPROTOCOL_VERSION = 2
HEARTBEAT_VERSION   = 1


def header_get_type(header):
    return (header >> 24) & 0xFF

def header_get_size(header):
    return header & 0x00FFFFFF


class FTDIDevice:
    """Wrapper around pyftdi for flashcart USB communication."""

    def __init__(self, device_index=0):
        self.ftdi = Ftdi()
        self.device_index = device_index
        self.cart_type = None

    def open(self):
        """Open the FTDI device."""
        devices = Ftdi.list_devices()
        if not devices:
            raise RuntimeError("No FTDI devices found. Is the flashcart connected?")

        if self.device_index >= len(devices):
            raise RuntimeError(f"Device index {self.device_index} out of range. "
                             f"Found {len(devices)} device(s).")

        desc, _ = devices[self.device_index]
        vid, pid = desc.vid, desc.pid
        print(f"Opening FTDI device {self.device_index}: VID=0x{vid:04x} PID=0x{pid:04x}")

        url = f"ftdi://0x{vid:04x}:0x{pid:04x}/{self.device_index + 1}"
        self.ftdi.open_from_url(url)
        self.ftdi.set_baudrate(115200)
        self.ftdi.set_latency_timer(2)
        self.ftdi.purge_buffers()

        # Identify cart type based on PID
        if pid == 0x6014:
            self.cart_type = "SC64"
        elif pid == 0x6001:
            self.cart_type = "64Drive/EverDrive"
        else:
            self.cart_type = "Unknown"

        print(f"Connected to: {self.cart_type}")

    def close(self):
        """Close the FTDI device."""
        try:
            self.ftdi.close()
        except Exception:
            pass

    def read(self, size, timeout=1.0):
        """Read data from USB with timeout."""
        data = bytearray()
        deadline = time.monotonic() + timeout
        while len(data) < size and time.monotonic() < deadline:
            chunk = self.ftdi.read_data(size - len(data))
            if chunk:
                data.extend(chunk)
            else:
                time.sleep(0.001)
        return bytes(data)

    def write(self, data):
        """Write data to USB."""
        self.ftdi.write_data(data)

    def read_header(self, timeout=0.1):
        """Try to read a 4-byte USB header. Returns (type, size) or None."""
        data = self.read(4, timeout=timeout)
        if len(data) < 4:
            return None
        header = struct.unpack(">I", data)[0]
        return header_get_type(header), header_get_size(header)

    def read_packet(self, size, timeout=1.0):
        """Read packet payload of given size."""
        # Round up to 4-byte alignment
        aligned = (size + 3) & ~3
        return self.read(aligned, timeout=timeout)[:size]


class N64Bridge:
    """Bridges USB (N64 flashcart) to UDP (game server)."""

    def __init__(self, server_host, server_port, device_index=0):
        self.server_host = server_host
        self.server_port = server_port
        self.device = FTDIDevice(device_index)
        self.udp_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.udp_socket.setblocking(False)
        self.running = False
        self.usb_to_server = deque(maxlen=256)
        self.server_to_usb = deque(maxlen=256)
        self.stats = {"usb_rx": 0, "usb_tx": 0, "udp_rx": 0, "udp_tx": 0}

    def start(self):
        """Start the bridge."""
        print(f"Server: {self.server_host}:{self.server_port}")
        self.device.open()
        self.running = True

        # Start USB reader thread
        usb_thread = threading.Thread(target=self._usb_loop, daemon=True)
        usb_thread.start()

        # Start UDP reader thread
        udp_thread = threading.Thread(target=self._udp_loop, daemon=True)
        udp_thread.start()

        # Main loop: forward packets between queues
        print("Bridge running. Press Ctrl+C to stop.")
        try:
            while self.running:
                # Forward USB → UDP
                while self.usb_to_server:
                    pkt = self.usb_to_server.popleft()
                    try:
                        self.udp_socket.sendto(pkt, (self.server_host, self.server_port))
                        self.stats["udp_tx"] += 1
                    except Exception as e:
                        print(f"UDP send error: {e}")

                # Forward UDP → USB
                while self.server_to_usb:
                    pkt = self.server_to_usb.popleft()
                    try:
                        # Wrap as DATATYPE_NETPACKET for USB
                        header = struct.pack(">I", (DATATYPE_NETPACKET << 24) | (len(pkt) & 0xFFFFFF))
                        self.device.write(header + pkt)
                        self.stats["usb_tx"] += 1
                    except Exception as e:
                        print(f"USB write error: {e}")

                time.sleep(0.001)  # 1ms poll

        except KeyboardInterrupt:
            print("\nStopping bridge...")
        finally:
            self.running = False
            self.device.close()
            self.udp_socket.close()
            self._print_stats()

    def _usb_loop(self):
        """Read packets from N64 via USB."""
        print("USB reader started")
        while self.running:
            try:
                result = self.device.read_header(timeout=0.05)
                if result is None:
                    continue

                dtype, size = result

                if dtype == DATATYPE_HEARTBEAT:
                    payload = self.device.read_packet(size)
                    if len(payload) >= 4:
                        proto = (payload[0] << 8) | payload[1]
                        hb_ver = (payload[2] << 8) | payload[3]
                        print(f"N64 heartbeat: protocol v{proto}, heartbeat v{hb_ver}")
                        # Send heartbeat response
                        resp = struct.pack(">I", (DATATYPE_HEARTBEAT << 24) | 4)
                        resp += struct.pack(">HH", USBPROTOCOL_VERSION, HEARTBEAT_VERSION)
                        self.device.write(resp)
                    self.stats["usb_rx"] += 1

                elif dtype == DATATYPE_NETPACKET:
                    payload = self.device.read_packet(size)
                    if payload:
                        self.usb_to_server.append(payload)
                        self.stats["usb_rx"] += 1
                        if self.stats["usb_rx"] % 100 == 0:
                            self._print_stats()

                elif dtype == DATATYPE_TEXT:
                    payload = self.device.read_packet(size)
                    if payload:
                        text = payload.decode("utf-8", errors="replace").rstrip("\x00")
                        print(f"N64: {text}")
                    self.stats["usb_rx"] += 1

                else:
                    # Skip unknown packet types
                    if size > 0:
                        self.device.read_packet(size)
                    self.stats["usb_rx"] += 1

            except Exception as e:
                if self.running:
                    print(f"USB read error: {e}")
                    time.sleep(0.1)

    def _udp_loop(self):
        """Read packets from server via UDP."""
        print("UDP reader started")
        while self.running:
            try:
                data, addr = self.udp_socket.recvfrom(4096)
                if data:
                    self.server_to_usb.append(data)
                    self.stats["udp_rx"] += 1
            except BlockingIOError:
                time.sleep(0.005)
            except Exception as e:
                if self.running:
                    print(f"UDP read error: {e}")
                    time.sleep(0.1)

    def _print_stats(self):
        s = self.stats
        print(f"[Stats] USB rx:{s['usb_rx']} tx:{s['usb_tx']} | "
              f"UDP rx:{s['udp_rx']} tx:{s['udp_tx']}")


def main():
    parser = argparse.ArgumentParser(description="N64 USB↔UDP Bridge for NetLib netplay")
    parser.add_argument("--server", "-s", default="127.0.0.1:6464",
                       help="Server address (default: 127.0.0.1:6464)")
    parser.add_argument("--device", "-d", type=int, default=0,
                       help="FTDI device index (default: 0)")
    parser.add_argument("--list", "-l", action="store_true",
                       help="List available FTDI devices and exit")
    args = parser.parse_args()

    if args.list:
        devices = Ftdi.list_devices()
        if not devices:
            print("No FTDI devices found.")
        else:
            for i, (desc, _) in enumerate(devices):
                print(f"  [{i}] VID=0x{desc.vid:04x} PID=0x{desc.pid:04x} "
                      f"SN={desc.sn or 'N/A'}")
        return

    host, port = args.server.rsplit(":", 1)
    bridge = N64Bridge(host, int(port), args.device)
    bridge.start()


if __name__ == "__main__":
    main()
