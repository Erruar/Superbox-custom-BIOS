#!/usr/bin/env python3
"""Drive the QEMU instance started by tools/qemu_run.ps1 over QMP.

Grabs screen dumps and injects keyboard/mouse events so the custom setup UI
can be validated headlessly.

Usage examples:
  py tools/qemu_probe.py dump screen0.ppm
  py tools/qemu_probe.py key down
  py tools/qemu_probe.py key down down enter
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
                return msg

    def screendump(self, path: str):
        self._cmd({"execute": "screendump", "arguments": {"filename": path}})

    def keys(self, names):
        for n in names:
            self._cmd({"execute": "input-send-event",
                       "arguments": {"events": [
                           {"type": "keydown", "data": {"key": {"qcode": n}}}]}})
            self._cmd({"execute": "input-send-event",
                       "arguments": {"events": [
                           {"type": "keyup", "data": {"key": {"qcode": n}}}]}})
            time.sleep(0.15)

    SHIFT_CHARS = {
        ':': 'semicolon', '.': 'dot', '_': 'minus', '+': 'equal', ')': '0',
        '(': '9', '&': '7', '%': '5', '$': '4', '#': '3', '@': '2', '!': '1',
        '?': 'slash', '"': 'quote', '<': 'comma', '>': 'period', '|': 'backslash',
        '~': 'grave_accent', '^': '6', '*': '8', '/': 'slash',
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
            key = {"qcode": q}
            ev = []
            if shift:
                ev.append({"type": "keydown", "data": {"key": {"qcode": "shift"}}})
            ev.append({"type": "keydown", "data": {"key": key}})
            ev.append({"type": "keyup", "data": {"key": key}})
            if shift:
                ev.append({"type": "keyup", "data": {"key": {"qcode": "shift"}}})
            self._cmd({"execute": "input-send-event", "arguments": {"events": ev}})
            time.sleep(0.05)

    def click(self, x: int, y: int, button: str = "left"):
        # QMP absolute pointer: 0..(screen width/height - 1) scaled to 0..(2^31)
        def scale(v):
            return int(v * 32767 / 1920)   # 1080p default; OVMF posts 1920x1080
        self._cmd({"execute": "input-send-event",
                   "arguments": {"events": [
                       {"type": "abs-move", "data": {"x": scale(x), "y": scale(y)}}]}})
        self._cmd({"execute": "input-send-event",
                   "arguments": {"events": [
                       {"type": "down", "data": {"button": button}}]}})
        time.sleep(0.1)
        self._cmd({"execute": "input-send-event",
                   "arguments": {"events": [
                       {"type": "up", "data": {"button": button}}]}})


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
