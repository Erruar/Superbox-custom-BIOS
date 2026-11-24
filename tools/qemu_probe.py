#!/usr/bin/env python3
r"""Drive the QEMU instance started by tools/qemu_run.ps1 over QMP.

Grabs screen dumps and injects keyboard/mouse events so the custom setup UI
can be validated headlessly.  Uses the QEMU 11 input-send-event schema:
  key:  {"type":"key","data":{"data":{"key":{"type":"qcode","data":"a"},"down":true}}}
  btn:  {"type":"btn","data":{"data":{"button":"left","down":true}}}
  move: {"type":"abs","data":{"data":{"axis":"x","value":N}}}

Usage examples:
  py tools/qemu_probe.py dump screen0.ppm
  py tools/qemu_probe.py key down enter
  py tools/qemu_probe.py text 'fs0:\EFI\BOOT\BOOTX64.EFI'
  py tools/qemu_probe.py click 800 400
  py tools/qemu_probe.py serial
"""
import json
import socket
import sys
import time
from pathlib import Path

HOST, PORT = "127.0.0.1", 4545


class Qmp:
    def __init__(self):
        self.s = socket.create_connection((HOST, PORT), timeout=10)
        self.f = self.s.makefile("rwb")
        self._read()                      # greeting
        self._cmd({"execute": "qmp_capabilities"})

    def _read(self):
        line = self.f.readline()
        if not line:
            raise RuntimeError("QMP connection closed")
        return json.loads(line.decode("utf-8"))

    def _cmd(self, obj):
        self.f.write((json.dumps(obj) + "\n").encode("utf-8"))
        self.f.flush()
        while True:
            msg = self._read()
            if "return" in msg or "error" in msg:
                if "error" in msg:
                    print("QMP error:", json.dumps(msg["error"]))
                return msg

    def _key(self, qcode, down):
        self._cmd({"execute": "input-send-event",
                   "arguments": {"events": [
                       {"type": "key",
                        "data": {"key": {"type": "qcode", "data": qcode},
                                 "down": down}}]}})

    def screendump(self, path: str):
        self._cmd({"execute": "screendump", "arguments": {"filename": path}})

    def key(self, qcode: str):
        self._key(qcode, True)
        self._key(qcode, False)

    def keys(self, names):
        for n in names:
            self.key(n)
            time.sleep(0.15)

    SHIFT_CHARS = {
        ':': 'semicolon', '.': 'dot', '_': 'minus', '+': 'equal', ')': '0',
        '(': '9', '&': '7', '%': '5', '$': '4', '#': '3', '@': '2', '!': '1',
        '?': 'slash', '"': 'quote', '<': 'comma', '>': 'period', '|': 'backslash',
        '~': 'grave_accent', '^': '6', '*': '8',
    }

    def text(self, s: str):
        for ch in s:
            if ch == ' ':
                q, shift = 'space', False
            elif ch == '\\':
                q, shift = 'backslash', False
            elif ch in self.SHIFT_CHARS:
                q, shift = self.SHIFT_CHARS[ch], True
            elif ch.isupper():
                q, shift = ch.lower(), True
            elif ch.isalpha() or ch.isdigit():
                q, shift = ch, False
            else:
                continue
            if shift:
                self._key('shift', True)
            self._key(q, True)
            self._key(q, False)
            if shift:
                self._key('shift', False)
            time.sleep(0.05)

    def abs_move(self, x: int, y: int, screen_w: int = 1920, screen_h: int = 1080):
        nx = int(x * 65535 / (screen_w - 1))
        ny = int(y * 65535 / (screen_h - 1))
        self._cmd({"execute": "input-send-event",
                   "arguments": {"events": [
                       {"type": "abs", "data": {"axis": "x", "value": nx}},
                       {"type": "abs", "data": {"axis": "y", "value": ny}}]}})

    def button(self, name: str, down: bool):
        self._cmd({"execute": "input-send-event",
                   "arguments": {"events": [
                       {"type": "btn",
                        "data": {"button": name, "down": down}}]}})

    def click(self, x: int, y: int, button: str = "left"):
        self.abs_move(x, y)
        self.button(button, True)
        time.sleep(0.1)
        self.button(button, False)


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 1
    q = Qmp()

    if args[0] == "dump":
        out = str(Path(args[1]).resolve()) if len(args) > 1 else "screen.ppm"
        q.screendump(out)
        print("wrote", out)
    elif args[0] == "text":
        q.text(args[1])
        print("typed:", args[1])
    elif args[0] == "key":
        q.keys(args[1:])
        print("keys sent:", " ".join(args[1:]))
    elif args[0] == "move":
        q.abs_move(int(args[1]), int(args[2]))
        print("move", args[1], args[2])
    elif args[0] == "click":
        q.click(int(args[1]), int(args[2]))
        print("click", args[1], args[2])
    elif args[0] == "serial":
        log = Path("C:/dev/sbx/qemu/serial.log")
        print(log.read_text(errors="replace") if log.exists() else "(no serial log)")
    else:
        print("unknown command", args[0])
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
