import csv, gzip, subprocess, sys, tempfile
from pathlib import Path
here=Path(__file__).resolve().parent
with tempfile.TemporaryDirectory() as directory:
    tmp=Path(directory)
    with gzip.open(sys.argv[1],"rt") as inp:
        (tmp/"records").write_bytes(b"".join(bytes(map(int,row)) for row in csv.reader(inp)))
    subprocess.run(["cc","-O3","-I",here.parent/"native",here/"digits.c",here.parent/"target/release/libverdant_memory.a",
                    "-pthread","-ldl","-lm","-o",tmp/"digits"],check=True)
    subprocess.run([tmp/"digits",tmp/"records"],check=True)
