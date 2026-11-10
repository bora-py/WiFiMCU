#!/usr/bin/env python3
"""
WiFiMCU v1.0.0 Firmware Flashing Utility (YModem over Serial)
Author: bora-py (2026)
"""

import sys
import os
import time
import serial
from ymodem.Socket import ModemSocket

DEFAULT_BIN = os.path.join(
    os.path.dirname(__file__), "..", "Build", "COM.MXCHIP.BASIC", "Output", "MiCOKit-3165", "WiFiMCU_Firmware_v1.0.0.bin"
)

def progress(task_index, file_name, file_size, sent_size):
    pct = int(sent_size * 100 / file_size)
    print(f"\rFlashing: {sent_size} / {file_size} bytes (%{pct})", end="", flush=True)

def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM3"
    bin_path = sys.argv[2] if len(sys.argv) > 2 else os.path.abspath(DEFAULT_BIN)

    if not os.path.isfile(bin_path):
        print(f"Error: Binary file not found at: {bin_path}")
        sys.exit(1)

    print(f"Opening port {port} at 115200 baud...")
    ser = serial.Serial(port, 115200, timeout=1)

    def read_fn(size, timeout=1):
        ser.timeout = timeout
        return ser.read(size)

    def write_fn(data, timeout=1):
        ser.timeout = timeout
        return ser.write(data)

    ser.reset_input_buffer()
    ser.reset_output_buffer()

    # Wake up bootloader menu
    ser.write(b"\r\n")
    time.sleep(0.3)
    buf = ser.read(ser.in_waiting)
    
    # Send FWUPDATE command ('1')
    ser.write(b"1\r\n")
    time.sleep(0.5)
    ser.reset_input_buffer()

    print(f"Starting YModem transfer for: {os.path.basename(bin_path)} ({os.path.getsize(bin_path)} bytes)...")
    modem = ModemSocket(read_fn, write_fn)
    ok = modem.send([bin_path], callback=progress)

    if ok:
        print("\n\n[SUCCESS] Firmware v1.0.0 flashed successfully!")
        time.sleep(1)
        ser.write(b"6\r\n") # Option 6: BOOT
        time.sleep(1)
        print("Boot Output:\n" + ser.read(ser.in_waiting).decode('utf-8', errors='ignore'))
    else:
        print("\n\n[ERROR] Flashing failed or was aborted.")

    ser.close()

if __name__ == "__main__":
    main()

