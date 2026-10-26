"""Regenerate tables and standalone plots from retained benchmark result.json files."""
import argparse
import sys
from pathlib import Path
sys.dont_write_bytecode = True
from harness.report import write_report

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--raw", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--include-smoke", action="store_true")
    parser.add_argument("--no-plots", action="store_true")
    args = parser.parse_args()
    write_report(args.raw, args.output, args.include_smoke, not args.no_plots)
