"""Run against two private daemons; never contact the user's buses."""
import os
import subprocess
import sys
import tempfile

with tempfile.TemporaryDirectory(prefix="helix-dbus-") as directory:
    daemons = []
    try:
        addresses = []
        for name in ("session", "system"):
            daemon = subprocess.Popen(
                [sys.argv[1], "--session", "--nofork", "--print-address=1",
                 f"--address=unix:path={directory}/{name}"],
                stdout=subprocess.PIPE, text=True,
                env=dict(os.environ, XDG_RUNTIME_DIR=directory),
            )
            daemons.append(daemon)
            address = daemon.stdout.readline().strip()
            if not address:
                raise RuntimeError("test daemon failed to start")
            addresses.append(address)
        environment = dict(os.environ, DBUS_SESSION_BUS_ADDRESS=addresses[0],
                           DBUS_SYSTEM_BUS_ADDRESS=addresses[1])
        result = subprocess.run([sys.argv[2]], env=environment, timeout=30)
    finally:
        for daemon in daemons:
            daemon.terminate()
        for daemon in daemons:
            daemon.wait(timeout=5)
sys.exit(result.returncode)
