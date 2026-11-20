"""Read config/secrets.local. Values are never printed."""
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SECRETS = ROOT / "config" / "secrets.local"


def load():
    cfg = {}
    for line in SECRETS.read_text().splitlines():
        line = line.strip()
        if line and not line.startswith("#") and "=" in line:
            k, v = line.split("=", 1)
            cfg[k.strip()] = v.strip()
    return cfg
