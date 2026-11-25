"""Read and publish the system-memory capacity declared by SMBIOS type 16."""
import json
import os
from pathlib import Path
import tempfile

MIB = 1 << 20


def dmi_capacity(table):
    """Sum SMBIOS type 16 system-memory arrays, in whole MiB."""
    offset = total = 0
    seen = set()
    while offset + 4 <= len(table):
        kind, length = table[offset:offset + 2]
        if length < 4 or offset + length > len(table):
            return None
        record = table[offset:offset + length]
        end = table.find(b"\x00\x00", offset + length)
        if end < 0:
            return None
        offset = end + 2
        if kind == 127:
            return total // MIB or None
        if kind != 16:
            continue
        if length < 15:
            return None
        if record[5] in (1, 2):  # Other or unknown use cannot establish a complete total.
            return None
        if record[5] != 3:
            continue
        handle = record[2:4]
        if handle in seen:
            return None
        seen.add(handle)
        kib = int.from_bytes(record[7:11], "little")
        if kib == 0x80000000:
            if length < 23:
                return None
            size = int.from_bytes(record[15:23], "little")
        elif kib < 0x80000000:
            size = kib * 1024
        else:
            return None
        if not size:
            return None
        total += size
    return None


DMI_TABLE = Path("/sys/firmware/dmi/tables/DMI")
FIRMWARE_RECORD = Path("/run/vram-platform/capacity.json")
BOOT_ID = Path("/proc/sys/kernel/random/boot_id")


def publish_platform():
    # systemd creates the root-owned runtime directory. Publish only the
    # aggregate capacity, atomically, so VM users never need the raw table.
    total = dmi_capacity(DMI_TABLE.read_bytes())
    if not total:
        FIRMWARE_RECORD.unlink(missing_ok=True)
        return
    data = json.dumps(dict(boot_id=BOOT_ID.read_text().strip(), platform_max_mib=total))
    with tempfile.NamedTemporaryFile(mode="w", dir=FIRMWARE_RECORD.parent, delete=False) as tmp:
        name = Path(tmp.name)
        try:
            tmp.write(data)
            tmp.flush()
            os.fchmod(tmp.fileno(), 0o644)
            name.replace(FIRMWARE_RECORD)
        finally:
            name.unlink(missing_ok=True)


def firmware_capacity():
    try:
        return dmi_capacity(DMI_TABLE.read_bytes()), "SMBIOS type 16"
    except FileNotFoundError:
        return None, None
    except PermissionError:
        pass
    try:
        cached = json.loads(FIRMWARE_RECORD.read_text())
        if isinstance(cached, dict) and cached.get("boot_id") == BOOT_ID.read_text().strip():
            bound = cached.get("platform_max_mib")
            if type(bound) is int and 0 < bound < 1 << 64:
                return bound, "SMBIOS type 16 (boot record)"
    except (FileNotFoundError, PermissionError, ValueError):
        pass
    return None, None
