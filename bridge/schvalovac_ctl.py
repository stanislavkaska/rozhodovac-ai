#!/usr/bin/env python3
"""Rozhodovač — původní název ovládacího skriptu, alias k rozhodovac_ctl.py."""
import runpy
from pathlib import Path

target = Path(__file__).resolve().parent / "rozhodovac_ctl.py"
runpy.run_path(str(target), run_name="__main__")
