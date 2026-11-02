#!/usr/bin/env python3
"""Run one shell command on the board via telnet. Usage: ota-lab-telnet.py HOST 'CMD'"""
import os
import sys
import telnetlib

PROMPT = b"__OTALAB__> "


def main():
    if len(sys.argv) < 3:
        sys.stderr.write("usage: ota-lab-telnet.py HOST COMMAND\n")
        return 2
    host = sys.argv[1]
    command = sys.argv[2]
    timeout = int(os.environ.get("OTA_TELNET_TIMEOUT", "120"))

    tn = telnetlib.Telnet(host, 23, timeout=10)
    tn.read_until(b"login:", timeout=15)
    tn.write(b"root\n")
    buf = tn.read_until(b"# ", timeout=15)
    if b"Password" in buf or b"password" in buf:
        tn.write(b"\n")
        tn.read_until(b"# ", timeout=15)

    tn.write(b"stty -echo\n")
    tn.read_until(b"# ", timeout=10)
    tn.write(b"PS1='__OTALAB__> '\n")
    tn.read_until(PROMPT, timeout=10)

    tn.write((command + "; echo __OTALAB_RC=$?\n").encode("ascii", errors="replace"))
    body = tn.read_until(PROMPT, timeout=timeout)
    text = body.decode("utf-8", errors="replace")
    if text.endswith("__OTALAB__> "):
        text = text[: -len("__OTALAB__> ")]

    lines = []
    rc = 1
    for ln in text.splitlines():
        stripped = ln.strip()
        if not stripped or stripped == command.strip():
            continue
        if stripped.startswith("__OTALAB_RC="):
            try:
                rc = int(stripped.split("=", 1)[1])
            except ValueError:
                rc = 1
            continue
        if stripped.startswith("PS1=") or stripped == "stty -echo":
            continue
        lines.append(ln)

    out = "\n".join(lines).strip()
    if out:
        sys.stdout.write(out + "\n")
    tn.write(b"exit\n")
    tn.close()
    return rc


if __name__ == "__main__":
    sys.exit(main())
