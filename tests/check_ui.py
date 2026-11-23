#!/usr/bin/env python3
import json
import os
import re
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
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


def translation_keys(messages, prefix=""):
    keys = set()
    for key, value in messages.items():
        path = f"{prefix}{key}"
        if isinstance(value, dict):
            keys.update(translation_keys(value, path + "."))
        else:
            assert isinstance(value, str) and value.strip(), path
            keys.add(path)
    return keys


catalogs = {lang: (plugin / "translations" / f"{lang}.json").read_text() for lang in ("en", "pt-BR")}
english, portuguese = (translation_keys(json.loads(content)) for content in catalogs.values())
assert english == portuguese, f"Translation keys differ: {sorted(english ^ portuguese)}"

required_keys = set()
for setting in manifest["setting"]:
    required_keys.update((setting["label_key"], setting["description_key"]))
    required_keys.update(choice["label_key"] for choice in setting.get("options", ()))
for script in plugin.glob("*.luau"):
    required_keys.update(re.findall(r'\btr\(\s*"([^"]+)"', script.read_text()))
assert required_keys <= english, f"Missing translations: {sorted(required_keys - english)}"

print("Noctalia manifest, entry points and translations validated")

luau_bin = os.environ.get("LUAU_BIN") or shutil.which("luau")
if luau_bin:
    test_script = repo_root / "tests/test_ui.luau"
    assert test_script.is_file(), f"Test script missing: {test_script}"

    with tempfile.TemporaryDirectory(prefix="prisma-ui-") as temp:
        test_script = Path(shutil.copy(test_script, temp))
        sources_module = Path(temp) / "ui_sources.luau"
        service_text = (plugin / "prisma_service.luau").read_text()
        widget_text = (plugin / "prisma_widget.luau").read_text()
        panel_text = (plugin / "prisma_panel.luau").read_text()
        sources_content = (
            "return {\n"
            f"    service = [==[{service_text}]==],\n"
            f"    widget = [==[{widget_text}]==],\n"
            f"    panel = [==[{panel_text}]==],\n"
            "    translations = {\n"
            + "".join(f'        ["{lang}"] = [==[{content}]==],\n' for lang, content in catalogs.items())
            + "    },\n}\n"
        )
        sources_module.write_text(sources_content)

        print(f"Running Luau UI behavioral test suite using {luau_bin}...")
        subprocess.run([luau_bin, str(test_script)], cwd=repo_root, check=True)
elif os.environ.get("LUAU_REQUIRE_VM") == "1":
    print("Erro: LUAU_REQUIRE_VM=1 definido, mas binário 'luau' não foi encontrado em LUAU_BIN ou PATH", file=sys.stderr)
    sys.exit(1)
else:
    print("Aviso: o executor 'luau' não está instalado no host local; testes comportamentais ignorados.")
