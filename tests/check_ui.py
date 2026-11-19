#!/usr/bin/env python3
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tomllib

repo_root = Path(__file__).resolve().parents[1]
plugin = repo_root / "ui/prisma"
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

luau_bin = os.environ.get("LUAU_BIN") or shutil.which("luau")
if luau_bin:
    test_script = repo_root / "tests/test_ui.luau"
    assert test_script.is_file(), f"Test script missing: {test_script}"

    sources_module = repo_root / "tests/ui_sources.luau"
    try:
        service_text = (plugin / "prisma_service.luau").read_text()
        widget_text = (plugin / "prisma_widget.luau").read_text()
        panel_text = (plugin / "prisma_panel.luau").read_text()
        sources_content = (
            "return {\n"
            f"    service = [==[{service_text}]==],\n"
            f"    widget = [==[{widget_text}]==],\n"
            f"    panel = [==[{panel_text}]==],\n"
            "}\n"
        )
        sources_module.write_text(sources_content)

        print(f"Running Luau UI behavioral test suite using {luau_bin}...")
        subprocess.run([luau_bin, str(test_script)], cwd=repo_root, check=True)
    finally:
        if sources_module.exists():
            sources_module.unlink()
elif os.environ.get("LUAU_REQUIRE_VM") == "1":
    print("Erro: LUAU_REQUIRE_VM=1 definido, mas binário 'luau' não foi encontrado em LUAU_BIN ou PATH", file=sys.stderr)
    sys.exit(1)
else:
    print("Aviso: o executor 'luau' não está instalado no host local; testes comportamentais ignorados.")
