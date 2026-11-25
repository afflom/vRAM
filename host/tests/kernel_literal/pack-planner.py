"""Copy the installed planner's Python runtime into the disposable activation root."""
from pathlib import Path
import re
import runpy
import shutil
import subprocess
import sys

root = Path(sys.argv[1])
runpy.run_path("/usr/lib/vram/kernel-plan")
runpy.run_path("/usr/lib/vram/kernel-firmware.py")
paths = {Path(module.__file__) for module in list(sys.modules.values()) if getattr(module, "__file__", None)}
paths.discard(Path(__file__))
paths.add(Path(sys.executable))
for path in list(paths):
    if path == Path(sys.executable) or path.suffix == ".so":
        linked = subprocess.check_output(["ldd", path], text=True)
        paths.update(Path(item) for item in re.findall(r"(/[^\s]+)\s+\(", linked))
for path in paths:
    target = root / path.relative_to("/")
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(path, target)
    shutil.copymode(path, target)
