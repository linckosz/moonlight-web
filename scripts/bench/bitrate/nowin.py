"""Imported first by every bench script that starts other programs: on Windows, no child
process ever gets a console WINDOW. A new window opens on the primary screen, and during a
stream the primary screen is the virtual one being filmed (06/11 22:31, a scoring console)."""
import os
import subprocess

if os.name == "nt" and not getattr(subprocess.Popen, "_mw_nowin", False):
    _init = subprocess.Popen.__init__

    def __init__(self, *a, **k):
        k["creationflags"] = k.get("creationflags", 0) | 0x08000000  # CREATE_NO_WINDOW
        _init(self, *a, **k)

    subprocess.Popen.__init__ = __init__
    subprocess.Popen._mw_nowin = True
