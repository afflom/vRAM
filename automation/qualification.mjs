// Replay the installed full-capacity receipt against the verified Prism model.
import {readFileSync} from 'node:fs';
import {verifiedModel} from './verified-model.mjs';

// Keep integer tokens exact even when the filesystem budget exceeds 2^53 bytes.
const r = JSON.parse(readFileSync(process.argv[2], 'utf8'), (_, value, context) =>
    typeof value === 'number' && /^-?\d+$/.test(context.source) ? BigInt(context.source) : value);
const capacity = r.capacity;
const target = capacity.configured_target_mib;
if (typeof target !== 'bigint' || target <= 0n || capacity.feasible_mib !== target ||
    r.verified_bytes !== target * 1048576n || r.file_writes !== target * 256n ||
    !Array.isArray(r.reserved) || !r.reserved.every(v => typeof v.bytes === 'bigint' && v.bytes > 0n) ||
    r.reserved.reduce((total, volume) => total + volume.bytes, 0n) !== r.verified_bytes + 4096n)
    throw Error('Incomplete full-capacity readback or reservation');

const {model, address} = await verifiedModel();
async function dispatch(tag, values) {
    const request = Buffer.alloc(1 + 8 * values.length);
    request.write(tag);
    values.forEach((value, i) => {
        if (typeof value !== 'bigint' || value < 0n || value > 0xffffffffffffffffn)
            throw Error('Invalid qualification measurement');
        request.writeBigUInt64BE(value, 1 + 8 * i);
    });
    return new TextDecoder().decode(await model.dispatch(request));
}

// The Linux launcher uses the signed file-offset limit when RLIMIT_AS is unlimited.
const addressBytes = capacity.address_space_limit_bytes === null ? (1n << 63n) - 1n : capacity.address_space_limit_bytes;
const admission = await dispatch('C', [capacity.platform_max_mib, capacity.guest_max_mib, addressBytes,
    capacity.file_budget_bytes, target]);
if (admission !== 'yes') throw Error('Prism refused the reported full capacity: ' + admission);
const verdict = await dispatch('y', [target, r.read_ns, r.write_ns, r.min_read_mib_s, r.min_write_mib_s]);
if (!['budget', 'complete'].includes(verdict) || verdict !== r.verdict)
    throw Error('Prism qualification verdict differs from installed launcher');
console.log(JSON.stringify({model: address, target_mib: Number(target), verdict}));
