# SPDX-License-Identifier: GPL-3.0-or-later
"""Repository locations shared by the tools."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MODELS = ROOT / '.cache/models'
