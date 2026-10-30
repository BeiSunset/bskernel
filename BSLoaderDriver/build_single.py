#!/usr/bin/env python3
"""Build the single-file BSLoADERDriver loader.

Usage:
    python3 BSLoADERDriver/build_single.py <modules-dir> [output]

<modules-dir> contains bs-android*.ko files produced by the build workflow.
The generated script embeds every module as base64 and verifies sha256 at run
time; it is attached to the GitHub release by the package job.
"""

from __future__ import annotations

import base64
import hashlib
import os
import sys
from pathlib import Path

ORDER = [
    "bs-android12-5.10.ko",
    "bs-android13-5.15.ko",
    "bs-android14-6.1.ko",
    "bs-android15-6.6.ko",
    "bs-android16-6.12.ko",
    "bs-android17-6.18.ko",
]

ROOT = Path(__file__).resolve().parent
TEMPLATE = ROOT / "BSLoaderDriver.template.sh"


def build_extract(modules: list[tuple[str, bytes, str]]) -> str:
    lines = [
        "extract_modules() {",
        '    info "正在释放内置驱动（约 1.7 MB）..."',
        '    mkdir -p "$MODULES_DIR" || return 1',
    ]
    for index, (name, data, _sha) in enumerate(modules, 1):
        marker = f"B64_{index}"
        encoded = base64.encodebytes(data).decode("ascii").rstrip("\n")
        lines.append(f'    decode_to "$MODULES_DIR/{name}" <<\'{marker}\'')
        lines.append(encoded)
        lines.append(marker)
        lines.append(f'    if [ ! -s "$MODULES_DIR/{name}" ]; then')
        lines.append(f'        err "释放 {name} 失败"')
        lines.append("        return 1")
        lines.append("    fi")
    lines.append('    ok "内置驱动释放完成"')
    lines.append("    return 0")
    lines.append("}")
    return "\n".join(lines)


def main() -> int:
    modules_dir = Path(sys.argv[1] if len(sys.argv) > 1 else "out/bskernel-arm64-v8a/modules")
    output = Path(sys.argv[2] if len(sys.argv) > 2 else "BSLoaderDriver.sh")

    modules: list[tuple[str, bytes, str]] = []
    for name in ORDER:
        path = modules_dir / name
        if not path.is_file():
            continue
        data = path.read_bytes()
        modules.append((name, data, hashlib.sha256(data).hexdigest()))

    if not modules:
        raise SystemExit(f"no bs-android*.ko files found in {modules_dir}")

    template = TEMPLATE.read_text(encoding="utf-8")
    module_list = " ".join(name for name, _data, _sha in modules)
    hash_lines = "\n".join(f"{sha}  {name}" for name, _data, sha in modules)

    script = template.replace("@@MODULE_LIST@@", module_list)
    script = script.replace("@@EXTRACT_MODULES@@", build_extract(modules))
    script = script.replace("@@HASH_LINES@@", hash_lines)
    for marker in ("@@MODULE_LIST@@", "@@EXTRACT_MODULES@@", "@@HASH_LINES@@"):
        if marker in script:
            raise SystemExit(f"unreplaced marker: {marker}")

    output.write_text(script, encoding="utf-8", newline="\n")
    output.chmod(0o755)
    print(f"wrote {output} ({output.stat().st_size} bytes)")
    for name, data, sha in modules:
        print(f"  {name}: {len(data)} bytes, sha256={sha}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
