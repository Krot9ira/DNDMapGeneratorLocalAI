"""Locate the project root from anywhere inside the package.

The app and the command line tools may sit in different folders (repo layout vs
shipped package), so the root is found by walking up to the styles library
rather than assumed to be the file's own directory.
"""
from pathlib import Path


def project_root():
    here = Path(__file__).resolve().parent
    for candidate in (here, *here.parents):
        if (candidate / "styles" / "_base.json").exists():
            return candidate
    return here.parent


ROOT = project_root()
