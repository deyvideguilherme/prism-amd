#!/usr/bin/env python3
import json
from pathlib import Path
import tomllib

plugin = Path(__file__).resolve().parents[1] / "ui/prisma"
manifest = tomllib.loads((plugin / "plugin.toml").read_text())
assert manifest["id"] == "deyvideguilherme/prisma"
assert "prisma-ctl" in manifest["dependencies"]

for component in ("service", "widget", "panel"):
    for entry in manifest[component]:
        script = plugin / entry["entry"]
        assert script.parent == plugin and script.is_file(), script

for translation in sorted((plugin / "translations").glob("*.json")):
    messages = json.loads(translation.read_text())
    for setting in manifest["setting"]:
        keys = [setting["label_key"], setting["description_key"]]
        keys.extend(choice["label_key"] for choice in setting.get("options", ()))
        for key in keys:
            value = messages
            for part in key.split("."):
                value = value[part]
            assert isinstance(value, str) and value, (translation, key)

print("Noctalia manifest, entry points and translations validated")
