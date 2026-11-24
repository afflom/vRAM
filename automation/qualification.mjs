// Replay the installed full-capacity receipt against the verified Prism model.
import {readFileSync} from 'node:fs';
import {verifiedModel} from './verified-model.mjs';

const r = JSON.parse(readFileSync(process.argv[2], 'utf8'));
const target = r.capacity.configured_target_mib;
if (!Number.isSafeInteger(target) || target <= 0 || r.capacity.feasible_mib !== target ||
    r.verified_bytes !== target * 1048576 || r.file_writes !== target * 256 ||
    r.reserved.reduce((total, volume) => total + volume.bytes, 0) !== r.verified_bytes + 4096)
    throw Error('Incomplete full-capacity readback or reservation');
const {model, address} = await verifiedModel();
const q = Buffer.alloc(41);
q.write('y');
[target, r.read_ns, r.write_ns, r.min_read_mib_s, r.min_write_mib_s].forEach((value, i) => {
    if (!Number.isSafeInteger(value) || value < 0) throw Error('Invalid qualification measurement');
    q.writeBigUInt64BE(BigInt(value), 1 + 8 * i);
});
const verdict = new TextDecoder().decode(await model.dispatch(q));
if (!['budget', 'complete'].includes(verdict) || verdict !== r.verdict)
    throw Error('Prism qualification verdict differs from installed launcher');
console.log(JSON.stringify({model: address, target_mib: target, verdict}));
