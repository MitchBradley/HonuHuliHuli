# Persistent settings in a JSON file on the board's filesystem.
# Portable across MicroPython ports (unlike esp32.NVS).

import json
import os

PATH = "settings.json"

_cache = None


def _load():
    global _cache
    if _cache is None:
        try:
            with open(PATH) as f:
                _cache = json.load(f)
        except (OSError, ValueError):
            _cache = {}
    return _cache


def get(key, default=None):
    return _load().get(key, default)


def set(key, value):
    d = _load()
    if d.get(key) == value:
        return
    d[key] = value
    # Write a temp file and rename it over the old one, so a reset
    # mid-write cannot leave a truncated settings file.
    tmp = PATH + ".tmp"
    with open(tmp, "w") as f:
        json.dump(d, f)
    os.rename(tmp, PATH)
