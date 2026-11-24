"""Boot a Linux host under TCG to exercise userfaultfd and an inner QEMU."""
import argparse, gzip, lzma, hashlib, json, re, stat, subprocess, sys, tempfile
from pathlib import Path
here = Path(__file__).resolve().parent
device = here.parent

def archive(entries):
    data = bytearray()
    for ino, (name,(mode,payload,major,minor)) in enumerate(entries.items(),1):
        fields=(ino,mode,0,0,1,0,len(payload),0,0,major,minor,len(name)+1,0)
        data.extend(("070701"+ "".join(f"{i:08x}" for i in fields)+name+"\0").encode())
        data.extend(bytes(-len(data)%4)); data.extend(payload); data.extend(bytes(-len(data)%4))
    return gzip.compress(data,compresslevel=1,mtime=0)

def run(*args):
    subprocess.run(args,check=True)

def svm_entries(entries, directory):
    for name in ("irqbypass", "ccp", "kvm", "kvm-amd"):
        file, = directory.rglob(name + ".ko.xz")
        entries[name + ".ko"] = (stat.S_IFREG|0o444, lzma.decompress(file.read_bytes()), 0, 0)
    entries["dev/kvm"] = (stat.S_IFCHR|0o600, b"", 10, 232)

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ("qemu","kernel","bios-dir","digits","result"):
        parser.add_argument("--"+name,required=True,type=Path)
    parser.add_argument("--svm-modules", type=Path, help="test inner KVM on an emulated AMD SVM host with these matching kernel modules")
    a=parser.parse_args()
    accel = "kvm" if a.svm_modules else "tcg"
    with tempfile.TemporaryDirectory() as folder:
        tmp=Path(folder)
        run("cc","-static","-O2","-pthread",*(["-DTEST_SVM"] if a.svm_modules else []),"-I",device/"native",here/"init.c",device/"native/pager.c",
            device/"target/release/libverdant_memory.a","-ldl","-lm","-o",tmp/"init")
        run("strip",tmp/"init")
        rows=gzip.decompress(a.digits.read_bytes()).decode().splitlines()
        digits=b"".join(bytes(map(int,row.split(",")[:-1])) for row in rows)
        if len(digits)!=1797*64 or max(digits)!=16: raise ValueError("unexpected digits corpus")
        with a.kernel.open("rb") as f: kernel=f.read(4<<20)
        def digest(data):
            h=2166136261
            for b in data: h=((h^b)*16777619)&0xffffffff
            return h
        run("cc",f"-DDIGITS_HASH={digest(digits)}u",f"-DKERNEL_HASH={digest(kernel)}u",f"-DDIGITS_BYTES={len(digits)}u","-m32","-O2","-ffreestanding","-fno-pie","-fno-stack-protector","-c",here/"inner.c","-o",tmp/"inner.o")
        run("as","--32",here/"start.S","-o",tmp/"start.o")
        run("ld","-m","elf_i386","-T",here/"rom.ld",tmp/"start.o",tmp/"inner.o","-o",tmp/"inner.elf")
        run("objcopy","-O","binary",tmp/"inner.elf",tmp/"inner.rom")
        run("strip","-o",tmp/"qemu",a.qemu)
        entries={}
        def add_file(name,content,mode):
            for parent in reversed(Path(name).parents):
                if str(parent)!=".": entries.setdefault(str(parent),(stat.S_IFDIR|0o755,b"",0,0))
            entries[name]=(stat.S_IFREG|mode,content,0,0)
        for d in ["dev","proc","sys","tmp"]:
            entries[d]=(stat.S_IFDIR|0o755,b"",0,0)
        for d,maj,mnr in [("console",5,1),("ttyS0",4,64),("null",1,3),("zero",1,5),("urandom",1,9),("random",1,8)]:
            entries["dev/"+d]=(stat.S_IFCHR|0o666,b"",maj,mnr)
        if a.svm_modules:
            svm_entries(entries, a.svm_modules)
        libs=subprocess.check_output(["ldd",str(a.qemu)],text=True)
        for lib in re.findall(r"(/[^\s]+) \(0x",libs):
            add_file(lib.lstrip("/"),Path(lib).read_bytes(),0o755)
        for name in ["init","qemu","inner.rom"]:
            add_file(name,(tmp/name).read_bytes(),0o755)
        add_file("digits",digits,0o444)
        add_file("kernel-prefix",kernel,0o444)
        initial=bytearray(8<<20)
        initial[2<<20:(2<<20)+len(digits)]=digits
        initial[4<<20:8<<20]=kernel
        add_file("image",initial,0o444)
        add_file("firmware/kvmvapic.bin",(a.bios_dir/"kvmvapic.bin").read_bytes(),0o444)
        entries["TRAILER!!!"]=(0,b"",0,0)
        (tmp/"initrd").write_bytes(archive(entries))
        cmd=[a.qemu,"-accel","tcg","-smp","2","-m","768","-machine","pc","-cpu","max,vendor=AuthenticAMD,+svm" if a.svm_modules else "max",
             "-kernel",a.kernel,"-initrd",tmp/"initrd","-append","console=ttyS0 quiet nokaslr rdinit=/init panic=-1",
             "-L",a.bios_dir,"-nodefaults","-display","none","-monitor","none","-serial","stdio",
             "-device","isa-debug-exit,iobase=0xf4,iosize=4","-no-reboot"]
        proc=subprocess.run(cmd,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=180)
        log=proc.stdout.decode(errors="replace")
        print(log,flush=True)
        if proc.returncode!=1 or f"PASS nested {accel.upper()} main RAM and Linux pager" not in log: sys.exit(1)
        a.result.write_text(json.dumps({"accelerator":"Linux KVM on TCG-emulated AMD SVM" if a.svm_modules else "TCG inside TCG for inner VM","log":log,
            "qemu_sha256":hashlib.sha256(a.qemu.read_bytes()).hexdigest(),
            "store_library_sha256":hashlib.sha256((device/"target/release/libverdant_memory.a").read_bytes()).hexdigest(),
            "kernel_sha256":hashlib.sha256(a.kernel.read_bytes()).hexdigest(),"digits_gz_sha256":hashlib.sha256(a.digits.read_bytes()).hexdigest(),
            "qemu_version":subprocess.check_output([a.qemu,"--version"],text=True).splitlines()[0]},indent=2)+"\n")
if __name__=="__main__": main()
