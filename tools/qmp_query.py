#!/usr/bin/env python3
"""QMP query helper: send any query and print the JSON reply."""
import json
import socket
import sys

HOST, PORT = "127.0.0.1", 4545


def qmp(objs):
    s = socket.create_connection((HOST, PORT), timeout=10)
    f = s.makefile("rwb")
    f.readline()
    f.write((json.dumps({"execute": "qmp_capabilities"}) + "\n").encode())
    f.flush()
    f.readline()
    for o in objs:
        f.write((json.dumps(o) + "\n").encode())
        f.flush()
        while True:
            msg = json.loads(f.readline().decode())
            if "return" in msg or "error" in msg:
                print(json.dumps(msg, indent=1)[:2000])
                break
    s.close()


cmds = []
for q in sys.argv[1:]:
    cmds.append({"execute": q})
qmp(cmds)
