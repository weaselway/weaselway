#!/usr/bin/env python3
"""Keeps the listed USB devices attached to this WSL distribution.

Runs inside WSL as weaselway-usbipd-auto-attach.service, set up by the
weaselway.usbip.devices option. Needs the vhci_hcd module, which
weaselway-prep.service loads.
Usage: usbipd-auto-attach.py --hardware-ids 2357:0604,18d1:4eed [--usbipd PATH]
"""

import argparse
import re
import subprocess
import sys
import time
from dataclasses import dataclass
from datetime import datetime

# systemd services do not get the Windows PATH, so use the full path
DEFAULT_USBIPD = "/mnt/c/Program Files/usbipd-win/usbipd.exe"

# BUSID  VID:PID  DEVICE...  STATE
LINE_RE = re.compile(r"^(\S+)\s+([0-9a-fA-F]{4}:[0-9a-fA-F]{4})\s+(.+?)\s{2,}(\S.*?)\s*$")


@dataclass
class Device:
    bus_id: str
    hw_id: str
    name: str
    state: str


def parse_devices(output: str) -> list[Device]:
    """Returns the connected devices from `usbipd list` output.

    >>> parse_devices("Connected:\\nBUSID  VID:PID    DEVICE              STATE\\n"
    ...               "1-3    2357:0604  TP-Link Bluetooth   Shared\\n"
    ...               "\\nPersisted:\\nGUID  DEVICE\\n")
    [Device(bus_id='1-3', hw_id='2357:0604', name='TP-Link Bluetooth', state='Shared')]
    """
    devices: list[Device] = []
    for line in output.splitlines():
        if line.startswith("Persisted:"):
            break
        if m := LINE_RE.match(line):
            devices.append(Device(*m.groups()))
    return devices


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--hardware-ids", type=lambda s: s.lower().split(","), required=True)
    parser.add_argument("--usbipd", default=DEFAULT_USBIPD)
    args = parser.parse_args()

    while True:
        listing = subprocess.run([args.usbipd, "list"], capture_output=True, text=True, errors="replace")
        if listing.returncode != 0:
            print(f"usbipd list failed ({listing.returncode}): {listing.stderr.strip()}", file=sys.stderr)
            time.sleep(1)
            continue

        for device in parse_devices(listing.stdout):
            if device.hw_id.lower() not in args.hardware_ids or device.state == "Attached":
                continue

            print(
                f"{datetime.now():%X} attaching {device.bus_id} {device.hw_id} {device.name} ({device.state})",
                flush=True,
            )

            attach = subprocess.run([args.usbipd, "attach", "--wsl", "--busid", device.bus_id])
            if attach.returncode != 0:
                print(f"usbipd attach {device.bus_id} failed ({attach.returncode})", file=sys.stderr)
        time.sleep(1)


if __name__ == "__main__":
    main()
