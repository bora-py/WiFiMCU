#!/usr/bin/env python3
"""
WiFiMCU Community Edition - Flasher & Tool (wmcutool.py)
--------------------------------------------------------
High-speed, zero-corruption UART uploader & bytecode compiler for EMW3165 (WiFiMCU).
Author: Bora & AI Community
"""

import os
import sys
import time
import argparse
import serial

class WiFiMCUTool:
    def __init__(self, port='COM3', baud=115200, timeout=2.0):
        self.port = port
        self.baud = baud
        self.timeout = timeout
        self.s = None

    def connect(self):
        try:
            self.s = serial.Serial(self.port, self.baud, timeout=self.timeout)
            time.sleep(0.1)
            self.flush()
            self.send_cmd("", wait_prompt=True)
        except Exception as e:
            print(f"[-] Connection error on {self.port}: {e}")
            sys.exit(1)

    def close(self):
        if self.s and self.s.is_open:
            self.s.close()

    def flush(self):
        self.s.reset_input_buffer()
        self.s.reset_output_buffer()

    def send_cmd(self, cmd, wait_prompt=True, timeout=3.0):
        self.s.write((cmd + "\r\n").encode('ascii'))
        if not wait_prompt:
            return ""
        
        start = time.time()
        buf = bytearray()
        while time.time() - start < timeout:
            ch = self.s.read(1)
            if ch:
                buf.extend(ch)
                if buf.endswith(b"\r> ") or buf.endswith(b"\n> "):
                    return buf.decode('latin1', errors='ignore')
        return buf.decode('latin1', errors='ignore')

    def init_flash_helper(self):
        """Injects RAM helper function `_w(filename, hex_chunk)`"""
        self.send_cmd("collectgarbage('collect')")
        helper_code = (
            "function _w(f,h) "
            "file.open(f,'a+'); "
            "local t={}; "
            "for i=1,string.len(h),2 do "
            "table.insert(t,string.char(tonumber(string.sub(h,i,i+1),16))) "
            "end; "
            "file.write(table.concat(t)); "
            "file.close(); "
            "t=nil; "
            "end"
        )
        res = self.send_cmd(helper_code)
        if "error" in res.lower() or "unexpected" in res.lower():
            raise RuntimeError(f"Helper injection failed: {res}")

    def upload_file(self, local_path, remote_name=None, chunk_bytes=32):
        if not remote_name:
            remote_name = os.path.basename(local_path)

        if not os.path.exists(local_path):
            print(f"[-] File not found: {local_path}")
            return False

        with open(local_path, 'rb') as f:
            content = f.read()

        total = len(content)
        print(f"[*] Uploading '{local_path}' -> '{remote_name}' ({total} bytes)...")

        self.send_cmd(f"file.remove('{remote_name}')")
        self.send_cmd(f"file.open('{remote_name}', 'w+'); file.close()")

        start_t = time.time()
        for idx in range(0, total, chunk_bytes):
            chunk = content[idx:idx + chunk_bytes]
            hex_str = chunk.hex()
            self.send_cmd(f"_w('{remote_name}','{hex_str}')")
            pct = min(100, int((idx + len(chunk)) / total * 100))
            sys.stdout.write(f"\r    Progress: {pct}% [{idx + len(chunk)}/{total} bytes]")
            sys.stdout.flush()

        elapsed = time.time() - start_t
        speed = (total / elapsed) if elapsed > 0 else 0
        print(f"\n    [+] Done in {elapsed:.2f}s ({speed:.1f} B/s)")
        return True

    def compile(self, remote_lua):
        remote_lc = remote_lua.replace('.lua', '.lc')
        print(f"[*] Compiling on WiFiMCU: {remote_lua} -> {remote_lc}...")
        self.send_cmd("collectgarbage('collect')")
        res = self.send_cmd(f"file.compile('{remote_lua}')", timeout=5.0)
        
        list_res = self.send_cmd("print(file.list()['" + remote_lc + "'])")
        if "nil" in list_res:
            print(f"    [-] Compilation failed: {res}")
            return False

        print(f"    [+] Compiled successfully! Removing '{remote_lua}'...")
        self.send_cmd(f"file.remove('{remote_lua}')")
        return True

    def list_files(self):
        print("\n[*] Files on WiFiMCU SPIFFS:")
        out = self.send_cmd("for k,v in pairs(file.list()) do print('   ', k, v) end")
        print(out.replace("\r> ", "").strip())

    def format_fs(self):
        print("[!] Formatting SPIFFS on WiFiMCU (this takes ~3-4s)...")
        self.send_cmd("file.format()", timeout=6.0)
        print("    [+] Format complete.")

    def reboot(self):
        print("[*] Rebooting WiFiMCU...")
        self.send_cmd("if mcu and mcu.reboot then mcu.reboot() end", wait_prompt=False)
        time.sleep(1.5)


def main():
    parser = argparse.ArgumentParser(description="WiFiMCU Community Edition - Flasher & Tool")
    parser.add_argument('--port', default='COM3', help="Serial port (default: COM3)")
    parser.add_argument('--baud', type=int, default=115200, help="Baud rate (default: 115200)")
    
    subparsers = parser.add_subparsers(dest='command', help="Commands")

    # list
    subparsers.add_parser('ls', help="List files in SPIFFS")

    # format
    subparsers.add_parser('format', help="Format SPIFFS filesystem")

    # reboot
    subparsers.add_parser('reboot', help="Reboot WiFiMCU board")

    # upload
    up_parser = subparsers.add_parser('upload', help="Upload local file(s)")
    up_parser.add_argument('files', nargs='+', help="Files to upload")
    up_parser.add_argument('--compile', action='store_true', help="Compile .lua files to .lc after upload")

    # deploy (full station firmware setup)
    subparsers.add_parser('deploy', help="One-command build & deploy full station firmware (bmp + app + init)")

    args = parser.parse_args()

    if not args.command:
        parser.print_help()
        sys.exit(0)

    tool = WiFiMCUTool(port=args.port, baud=args.baud)
    tool.connect()

    if args.command == 'ls':
        tool.list_files()

    elif args.command == 'format':
        tool.format_fs()
        tool.list_files()

    elif args.command == 'reboot':
        tool.reboot()

    elif args.command == 'upload':
        tool.init_flash_helper()
        for f in args.files:
            remote_name = os.path.basename(f)
            if tool.upload_file(f, remote_name):
                if args.compile and f.endswith('.lua'):
                    tool.compile(remote_name)
        tool.list_files()

    elif args.command == 'deploy':
        print("=== ONE-CLICK DEPLOYING ROOM STATION FIRMWARE ===")
        tool.init_flash_helper()
        
        # 1. bmp module
        if os.path.exists("bmp.lua"):
            tool.upload_file("bmp.lua", "bmp.lua")
            tool.compile("bmp.lua")

        # 2. app_main module
        app_file = "app_main.lua" if os.path.exists("app_main.lua") else "app.lua"
        tool.upload_file(app_file, "app.lua")
        tool.compile("app.lua")

        # 3. minimal init.lua
        print("[*] Creating minimal bootloader 'init.lua'...")
        tool.send_cmd("file.open('init.lua', 'w+')")
        tool.send_cmd("file.writeline('collectgarbage(); dofile(\"app.lc\")')")
        tool.send_cmd("file.close()")
        print("    [+] init.lua created.")

        tool.list_files()
        tool.reboot()
        print("\n[+] FULL DEPLOY SUCCESSFUL! Station is booting...")

    tool.close()

if __name__ == '__main__':
    main()

