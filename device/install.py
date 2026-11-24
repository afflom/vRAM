"""Install the built static library and register the backend in QEMU 9.2.0."""
import pathlib, shutil, sys
here = pathlib.Path(__file__).resolve().parent
q = pathlib.Path(sys.argv[1])
if (q/'VERSION').read_text().strip() != '9.2.0':
    raise ValueError("expected QEMU 9.2.0")
for f in ['pager.c','pager.h','hostmem-tensor.c']:
    shutil.copy2(here/'native'/f, q/'backends'/f)
shutil.copy2(here/'target/release/libverdant_memory.a', q/'backends')
p = q/'backends/meson.build'
s = p.read_text()
if "'hostmem-tensor.c'" not in s:
    p.write_text(s + """
if host_os == 'linux'
  system_ss.add(files('hostmem-tensor.c', 'pager.c'),
                cc.find_library('verdant_memory', dirs: meson.current_source_dir()))
endif
""")
p = q/'qapi/qom.json'
s = p.read_text()
if "'memory-backend-verdant-tensor'" not in s:
    for a,b in [("    'memory-backend-ram',", "    { 'name': 'memory-backend-verdant-tensor', 'if': 'CONFIG_LINUX' },"),
                ("      'memory-backend-ram':         'MemoryBackendProperties',","      'memory-backend-verdant-tensor': { 'type': 'MemoryBackendProperties', 'if': 'CONFIG_LINUX' },")]:
        if s.count(a) != 1: raise ValueError("unexpected QAPI schema")
        s=s.replace(a,a+'\n'+b)
    p.write_text(s)

s = p.read_text()
if "{ 'struct': 'TensorRamProperties'," not in s:
    anchor = "##\n# @MemoryBackendFileProperties:"
    if s.count(anchor)!=1: raise ValueError("missing memory schema")
    decl = """##
# @TensorRamProperties:
#
# @image: optional byte-exact initial RAM image, zero-extended to size
#
# Since: 9.2
##
{ 'struct': 'TensorRamProperties', 'base': 'MemoryBackendProperties',
  'data': { '*image': 'str' }, 'if': 'CONFIG_LINUX' }

"""
    p.write_text(s.replace(anchor, decl+anchor).replace(
        "'memory-backend-verdant-tensor': { 'type': 'MemoryBackendProperties'",
        "'memory-backend-verdant-tensor': { 'type': 'TensorRamProperties'"))

# Upgrade an earlier installation as well as a fresh QEMU 9.2 checkout.
s = p.read_text()
anchor = "  'data': { '*image': 'str' }, 'if': 'CONFIG_LINUX' }"
if anchor in s:
    desc = "# @image: optional byte-exact initial RAM image, zero-extended to size"
    s = s.replace(desc, desc + "\n#\n# @x-quiescent-reclaim: operator guarantees exclusion of DMA and host writers\n#     for each requested range while the virtual CPUs are paused")
    p.write_text(s.replace(anchor, "  'data': { '*image': 'str', '*x-quiescent-reclaim': 'bool' }, 'if': 'CONFIG_LINUX' }"))
misc = q/'qapi/misc.json'
s = misc.read_text()
if "# Capture and discard a range of private vRAM on a paused TCG VM." in s:
    s = s.replace("Capture and discard a range of private vRAM on a paused TCG VM.",
                  "Reclaim a range of private vRAM on a paused TCG or capture-all KVM VM.").replace(
                  "populated pages captured and discarded", "populated pages discarded")
    misc.write_text(s)
if "'command': 'x-vram-reclaim'" not in s:
    misc.write_text(s + (here/'native/reclaim.qapi').read_text())
s = misc.read_text()
stats_schema = (here/'native/store-stats.qapi').read_text()
if "'command': 'x-query-vram-store'" in s:
    start = s.index('##\n# @VramStoreStats:')
    end = s.index("'features': ['unstable'] }", s.index("{ 'command': 'x-query-vram-store'", start))
    end += len("'features': ['unstable'] }")
    s = s[:start] + stats_schema.strip() + s[end:]
else:
    s += stats_schema
if s != misc.read_text():
    misc.write_text(s)
s = p.read_text()
if "'*x-resident-target'" not in s:
    anchor = "# @x-quiescent-reclaim: operator"
    if s.count(anchor) != 1: raise ValueError("unexpected reclaim schema")
    s = s.replace(anchor, "# @x-resident-target: soft resident frame target in bytes (zero disables automatic reclaim)\n#\n" + anchor)
    p.write_text(s.replace("'*x-quiescent-reclaim': 'bool'", "'*x-quiescent-reclaim': 'bool', '*x-resident-target': 'size'"))

s = p.read_text()
if "'*x-search-nodes'" not in s:
    anchor = "# @x-resident-target:"
    if s.count(anchor) != 1: raise ValueError("unexpected resident target schema")
    s = s.replace(anchor, "# @x-search-nodes: maximum tensor-plan subproblems per eligible page, 0..49\n#\n" + anchor)
    p.write_text(s.replace("'*x-resident-target': 'size'", "'*x-resident-target': 'size', '*x-search-nodes': 'uint32'"))

s = p.read_text()
if "'*x-adaptive-search'" not in s:
    marker = "# @x-search-nodes:"
    if s.count(marker) != 1: raise ValueError("unexpected search schema")
    s = s.replace(marker, "# @x-adaptive-search: reuse successful local program shapes and periodically explore (default on)\n#\n" + marker)
    p.write_text(s.replace("'*x-search-nodes': 'uint32'", "'*x-search-nodes': 'uint32', '*x-adaptive-search': 'bool'"))
