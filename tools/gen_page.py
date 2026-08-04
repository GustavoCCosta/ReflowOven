#!/usr/bin/env python3
"""Turn src/net/index.html into the C string served by the firmware.

Usage: python3 tools/gen_page.py  (run from the application root)
"""
import pathlib

src = pathlib.Path("src/net/index.html")
dst = pathlib.Path("src/net/index_html.h")

lines = []
for line in src.read_text(encoding="utf-8").splitlines():
    esc = line.replace("\\", "\\\\").replace('"', '\\"')
    lines.append(f'\t"{esc}\\n"')

dst.write_text(
    "/* SPDX-License-Identifier: Apache-2.0 */\n"
    "/* Generated from index.html by tools/gen_page.py - do not edit. */\n\n"
    "#ifndef REFLOW_INDEX_HTML_H_\n#define REFLOW_INDEX_HTML_H_\n\n"
    "static const char index_html[] =\n" + "\n".join(lines) + ";\n\n"
    "#endif /* REFLOW_INDEX_HTML_H_ */\n",
    encoding="utf-8",
)
print(f"{dst}: {dst.stat().st_size} bytes")
