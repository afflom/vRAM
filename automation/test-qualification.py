"""Exercise the receipt parser at a byte boundary above JavaScript's safe integer range."""
import json
from pathlib import Path
import subprocess
from tempfile import TemporaryDirectory

root = Path(__file__).resolve().parents[1]
source = root / "measurements/qualified-capacity-0.6.0.json"
replay = ["node", root / "automation/qualification.mjs"]
subprocess.run([*replay, source], check=True)
# Scale only the accounting inputs. This tests wire precision without allocating this memory.
receipt = json.loads(source.read_text())
target = 1 << 33
required = (target << 20) + 4096
capacity = receipt["capacity"]
capacity.update(platform_max_mib=target, guest_max_mib=target, configured_target_mib=target,
                feasible_mib=target, file_budget_bytes=required)
receipt.update(verified_bytes=target << 20, file_writes=target * 256, reserved=[{"bytes": required}])
with TemporaryDirectory() as directory:
    fixture = Path(directory) / "receipt.json"
    fixture.write_text(json.dumps(receipt))
    subprocess.run([*replay, fixture], check=True)
    capacity["file_budget_bytes"] -= 1
    fixture.write_text(json.dumps(receipt))
    result = subprocess.run([*replay, fixture], capture_output=True, text=True)
    if result.returncode == 0 or "Prism refused the reported full capacity: capacity mismatch" not in result.stderr:
        raise RuntimeError("expected refusal of a one-byte backing shortfall: " + result.stderr)
print("Verified exact and one-byte-short full-capacity receipt boundaries.")
