"""Install the .deb in a disposable Debian generic cloud image and boot it twice."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
for arg in ("image", "deb", "result"):
    parser.add_argument("--" + arg, required=True, type=Path)
parser.add_argument("--qemu", default="qemu-system-x86_64")
parser.add_argument("--genisoimage", default="genisoimage")
parser.add_argument("--bios-dir", type=Path)
args = parser.parse_args()
install = """#!/bin/sh
set -eu
exec </dev/null >/var/log/vram-installed-test.log 2>&1
trap 'cat /var/log/vram-installed-test.log >/dev/console; echo VRAM_DIST_FAILED >/dev/console; systemctl --no-block poweroff' EXIT
export SYSTEMD_PAGER=cat TERM=dumb
systemctl mask --now serial-getty@ttyS0.service
systemctl mask systemd-networkd-wait-online.service
mkdir -p /mnt/vram-test
mount -o ro /dev/sr0 /mnt/vram-test
dpkg -i /mnt/vram-test/vram.deb
# This disposable VM uses vRAM as its only swap path on both boots.
if test -e /sys/module/zswap/parameters/enabled; then
    echo 'w /sys/module/zswap/parameters/enabled - - - - 0' > /etc/tmpfiles.d/vram-test.conf
    systemd-tmpfiles --create /etc/tmpfiles.d/vram-test.conf
fi
systemctl enable --now vram@nbd0.service
systemctl is-active --quiet vram@nbd0.service
swapon --show
systemd-run --unit=vram-sqlite-test --wait --pipe -p MemoryMax=40M /usr/bin/python3 /mnt/vram-test/workload.py
systemctl kill --kill-whom=main --signal=USR1 vram@nbd0.service
sleep 1
journalctl -o cat -u vram@nbd0.service | tail -8
systemctl stop vram@nbd0.service
test -z "$(swapon --noheadings --show=NAME | grep /dev/nbd0 || :)"
cp /mnt/vram-test/second.service /etc/systemd/system/vram-reboot-test.service
systemctl daemon-reload
systemctl enable vram-reboot-test.service
echo VRAM_INSTALLED_AND_DRAINED
cat /var/log/vram-installed-test.log >/dev/console
trap - EXIT
systemctl --no-block reboot
"""
second = """[Unit]
Description=Verify automatic vRAM activation after reboot
After=vram@nbd0.service
Wants=vram@nbd0.service
[Service]
Type=oneshot
StandardOutput=tty
TTYPath=/dev/ttyS0
ExecStart=/bin/sh -c 'if systemctl is-active --quiet vram@nbd0.service && grep -q /dev/nbd0 /proc/swaps; then echo VRAM_REBOOT_PASSED; else echo VRAM_REBOOT_FAILED; fi; systemctl --no-block poweroff'
[Install]
WantedBy=multi-user.target
"""
workload = '''import hashlib, json, random, sqlite3
from pathlib import Path
def counter():
    return dict(line.split() for line in Path("/proc/vmstat").read_text().splitlines())
def blob(i):
    return random.Random(i).randbytes(4096)
before=counter()
db=sqlite3.connect(":memory:")
db.execute("CREATE TABLE buffers(i INTEGER PRIMARY KEY, payload BLOB)")
rows=20480
db.executemany("INSERT INTO buffers VALUES (?,?)", ((i,blob(i)) for i in range(rows)))
for i in range(0, rows, 11):
    db.execute("UPDATE buffers SET payload=? WHERE i=?", (blob(i+rows), i))
db.commit()
checked=0
for i, value in db.execute("SELECT i,payload FROM buffers ORDER BY i"):
    assert value == blob(i+rows if i%11==0 else i)
    checked+=len(value)
assert checked == 4096*rows and db.execute("PRAGMA integrity_check").fetchone() == ("ok",)
after=counter()
result=dict(rows=rows, bytes_checked=checked, sqlite=sqlite3.sqlite_version,
            writes=int(after["pswpout"])-int(before["pswpout"]), reads=int(after["pswpin"])-int(before["pswpin"]))
assert min(result["writes"],result["reads"]) > 0
print("VRAM_SQLITE",json.dumps(result),flush=True)
'''
with tempfile.TemporaryDirectory() as temp:
    tmp = Path(temp)
    for name, content in {"user-data":install, "meta-data":"instance-id: vram-package-test\nlocal-hostname: vram\n",
                          "network-config":"version: 2\nethernets: {}\n", "second.service":second,
                          "workload.py":workload}.items():
        (tmp/name).write_text(content)
    subprocess.run([args.genisoimage,"-quiet","-o",tmp/"seed.iso","-volid","cidata","-joliet","-rock","-graft-points",
                    *[name+"="+str(tmp/name) for name in ("user-data","meta-data","network-config","second.service","workload.py")],
                    "vram.deb="+str(args.deb.resolve())],check=True)
    argv = [args.qemu,"-machine","pc","-accel","tcg","-cpu","max","-smp","2","-m","1536","-nodefaults",
            "-drive",f"if=ide,index=0,format=qcow2,snapshot=on,file={args.image.resolve()}",
            "-drive",f"if=ide,index=2,media=cdrom,format=raw,readonly=on,file={tmp/'seed.iso'}",
            "-serial","stdio","-monitor","none","-display","none"]
    if args.bios_dir: argv.extend(["-L",str(args.bios_dir)])
    serial=tmp/"serial"
    print(f"Guest serial {serial}",flush=True)
    with serial.open("wb") as log:
        proc=subprocess.run(argv,stdout=log,stderr=subprocess.STDOUT,timeout=900,env=dict(os.environ,TMPDIR=str(tmp)))
    log=serial.read_text(errors="replace")
    match=re.search(r"VRAM_SQLITE (\{[^\n]+\})",log)
    if proc.returncode or not match or "VRAM_INSTALLED_AND_DRAINED" not in log or "VRAM_REBOOT_PASSED" not in log:
        raise SystemExit(log[-6000:])
    result=dict(workload=json.loads(match[1]),package_sha256=hashlib.sha256(args.deb.read_bytes()).hexdigest(),
                image_sha256=hashlib.file_digest(args.image.open('rb'),'sha256').hexdigest(),enabled_after_reboot=True,
                guest_ram_mib=1536,workload_memory_max_mib=40)
    args.result.write_text(json.dumps(result,indent=2)+"\n")
    print(json.dumps(result))
