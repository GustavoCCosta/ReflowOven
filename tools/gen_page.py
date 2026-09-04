#!/usr/bin/env python3
"""Turn src/net/index.html into the C string served by the firmware.

Usage: python3 tools/gen_page.py <index.html> <index_html.h>

Both paths are required, and that is deliberate (RFO-B25). The header used to
be generated in place, next to index.html, and committed - so keeping the two
in step was a manual habit rather than a build rule. It is now produced into
the build directory by cmake/index_html.cmake, which passes both paths.

A default that wrote back into src/net/ would be worse than useless here: a
quoted #include searches the including file's own directory first, so a stray
src/net/index_html.h left behind by a hand run would SHADOW the generated one
and serve a page nobody can see in the build output.
"""
import pathlib
import sys


def main(argv):
    if len(argv) != 3:
        print(f"usage: {argv[0]} <index.html> <index_html.h>", file=sys.stderr)
        return 2

    src = pathlib.Path(argv[1])
    dst = pathlib.Path(argv[2])

    lines = []
    for line in src.read_text(encoding="utf-8").splitlines():
        esc = line.replace("\\", "\\\\").replace('"', '\\"')
        lines.append(f'\t"{esc}\\n"')

    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_text(
        "/* SPDX-License-Identifier: Apache-2.0 */\n"
        "/* Generated from index.html by tools/gen_page.py - do not edit,\n"
        " * and do not commit: the build regenerates it (RFO-B25). */\n\n"
        "#ifndef REFLOW_INDEX_HTML_H_\n#define REFLOW_INDEX_HTML_H_\n\n"
        "static const char index_html[] =\n" + "\n".join(lines) + ";\n\n"
        "#endif /* REFLOW_INDEX_HTML_H_ */\n",
        encoding="utf-8",
    )
    print(f"{dst}: {dst.stat().st_size} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
