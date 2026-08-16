#!/usr/bin/env python3
"""Build the compact, ink-saving HIVE API PDF from its HTML source."""
from __future__ import annotations

import argparse
import sys
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", nargs="?", type=Path,
                        default=Path(__file__).parents[1] / "docs/HBOS_HAX_API.html")
    parser.add_argument("--output", "-o", type=Path,
                        default=Path(__file__).parents[1] / "HBOS_HAX_API.pdf")
    args = parser.parse_args()

    try:
        from weasyprint import HTML
    except ImportError:
        print("error: WeasyPrint is required; install it with 'pip install weasyprint'", file=sys.stderr)
        return 2

    if not args.source.is_file():
        print(f"error: HTML source not found: {args.source}", file=sys.stderr)
        return 2
    args.output.parent.mkdir(parents=True, exist_ok=True)
    document = HTML(filename=str(args.source),
                    base_url=str(args.source.parent)).render()
    document.write_pdf(str(args.output))
    print(f"generated {args.output} ({len(document.pages)} pages)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
