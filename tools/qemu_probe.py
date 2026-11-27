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
import os
import socket
import sys
import time
from pathlib import Path

HOST, PORT = "127.0.0.1", 4545

# Screenshots are test artifacts, never repository content (UI captures can
# show serials and other machine-specific data): every dump goes to the
# scratch directory, keeping only the requested file name.
DUMP_DIR = Path(os.environ.get("TEMP", "/tmp")) / "opencode"


def dump_path(name: str) -> str:
    DUMP_DIR.mkdir(parents=True, exist_ok=True)
    return str(DUMP_DIR / Path(name).name)


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

    def _raw(self, usage, down):
        """Send one key by number (USB HID usage) - layout independent."""
        self._cmd({"execute": "input-send-event",
                   "arguments": {"events": [
                       {"type": "key",
                        "data": {"key": {"type": "number", "data": usage},
                                 "down": down}}]}})

    # US keyboard: HID usage for the unshifted key of each character.
    US_SHIFT = {
        ')': 0x27, '!': 0x1E, '@': 0x1F, '#': 0x20, '$': 0x21, '%': 0x22,
        '^': 0x23, '&': 0x24, '*': 0x25, '(': 0x26, '_': 0x2D, '+': 0x2E,
        '{': 0x2A, '}': 0x2B, ':': 0x33, '"': 0x34, '|': 0x35,
        '~': 0x35, '<': 0x36, '>': 0x37, '?': 0x38,
    }
    US_PLAIN = {
        ' ': 0x2C, '-': 0x2D, '=': 0x2E, '[': 0x2A, ']': 0x2B, '\\': 0x2F,
        ';': 0x33, "'": 0x34, '`': 0x35, ',': 0x36, '.': 0x37, '/': 0x38,
    }
    NAMED = {
        'esc': 0x29, 'ret': 0x28, 'tab': 0x2B, 'bs': 0x2A,
        'up': 0x52, 'down': 0x51, 'right': 0x4F, 'left': 0x50,
        'home': 0x4A, 'end': 0x4D, 'pgup': 0x4B, 'pgdn': 0x4E,
        'del': 0x4C, 'ins': 0x49, 'space': 0x2C,
        'f1': 0x3A, 'f2': 0x3B, 'f3': 0x3C, 'f4': 0x3D, 'f5': 0x3E,
        'f6': 0x3F, 'f7': 0x40, 'f8': 0x41, 'f9': 0x42, 'f10': 0x43,
        'f11': 0x44, 'f12': 0x45,
    }

    # character -> (qcode, shift) on a US layout
    QSYM = {
        '-': ('minus', False), '=': ('equal', False), '[': ('bracket_left', False),
        ']': ('bracket_right', False), '\\': ('backslash', False),
        ';': ('semicolon', False), "'": ('apostrophe', False), '`': ('grave_accent', False),
        ',': ('comma', False), '.': ('dot', False), '/': ('slash', False),
        '!': ('1', True), '@': ('2', True), '#': ('3', True), '$': ('4', True),
        '%': ('5', True), '^': ('6', True), '&': ('7', True), '*': ('8', True),
        '(': ('9', True), ')': ('0', True), '_': ('minus', True), '+': ('equal', True),
        '{': ('bracket_left', True), '}': ('bracket_right', True),
        '|': ('backslash', True), ':': ('semicolon', True), '"': ('apostrophe', True),
        '~': ('grave_accent', True), '<': ('comma', True), '>': ('dot', True),
        '?': ('slash', True),
    }

    # QEMU qcode names (QKeyCode enum of this build), used for typing keys
    QKEYS = {
        'esc': 'esc', 'ret': 'ret', 'tab': 'tab', 'bs': 'backspace',
        'up': 'up', 'down': 'down', 'right': 'right', 'left': 'left',
        'home': 'home', 'end': 'end', 'pgup': 'pgup', 'pgdn': 'pgdn',
        'del': 'delete', 'ins': 'insert', 'space': 'spc',
        'f1': 'f1', 'f2': 'f2', 'f3': 'f3', 'f4': 'f4', 'f5': 'f5',
        'f6': 'f6', 'f7': 'f7', 'f8': 'f8', 'f9': 'f9', 'f10': 'f10',
        'f11': 'f11', 'f12': 'f12',
        'minus': 'minus', 'equal': 'equal',
    }

    def press_named(self, name, shift=False):
        q = self.QKEYS.get(name, name)
        if shift:
            self._key('shift', True)
        self._key(q, True)
        self._key(q, False)
        if shift:
            self._key('shift', False)

    def press(self, usage, shift=False):
        if shift:
            self._raw(0xE1, True)
        self._raw(usage, True)
        self._raw(usage, False)
        if shift:
            self._raw(0xE1, False)

    def screendump(self, path: str):
        self._cmd({"execute": "screendump", "arguments": {"filename": path}})

    def key(self, name: str):
        if name.isdigit():
            self.press(int(name))
            return
        if '+' in name:                       # combo, e.g. ctrl+c
            parts = name.split('+')
            mods, base = parts[:-1], parts[-1]
            for m in mods:
                self._key(m, True)
            self.press_named(base)
            for m in reversed(mods):
                self._key(m, False)
            return
        if name in self.QKEYS:
            self.press_named(name)
            return
        if len(name) == 1:
            self.char(name)
            return
        print("unknown key", name)

    def keys(self, names):
        for n in names:
            self.key(n)
            time.sleep(0.15)

    def char(self, ch):
        if ch == '\n':
            self.press_named('ret')
        elif ch.isalpha():
            self.press_named(ch.lower(), shift=ch.isupper())
        elif ch.isdigit():
            self.press_named(ch)
        elif ch in self.QSYM:
            q, shift = self.QSYM[ch]
            self.press_named(q, shift=shift)
        elif ch.isspace():
            self.press_named('space')
        else:
            print("skipped character", repr(ch))

    def text(self, s: str):
        for ch in s:
            self.char(ch)
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
        out = dump_path(args[1] if len(args) > 1 else "screen.ppm")
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
