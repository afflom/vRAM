// Replay the median-of-three contract on each operator and mixed-overwrite cell.
import {readFileSync, writeFileSync} from 'node:fs';
import {verifiedModel} from './verified-model.mjs';

const [input, output, gate] = process.argv.slice(2);
if (!input || !output || (gate && gate !== '--require-parity'))
    throw Error('Usage: node automation/swap-latency.mjs input.json output.json [--require-parity]');
const receipt = JSON.parse(readFileSync(input));
const {model, address} = await verifiedModel();
const backends = ['control', 'vram', 'zram-zstd'];
const sameKeys = (obj, keys) => JSON.stringify(Object.keys(obj).sort()) === JSON.stringify([...keys].sort());
if (receipt.page_bytes !== 4096 || receipt.queue_depth !== 1 || receipt.runs !== 3 ||
    receipt.samples_per_run !== 2048 || receipt.verified_bytes_per_backend !== 3 * 2 * 2048 * 4096 ||
    receipt.mixed_pages_per_family_per_round !== 64 || receipt.mixed_verified_bytes_per_backend !== 3 * 6 * 64 * 4096)
    throw Error('Invalid workload dimensions or readback count');

async function certify(suite, profiles) {
    const cells = profiles.flatMap(profile => ['write', 'read'].map(op => profile + '_' + op));
    if (!sameKeys(suite, cells)) throw Error('Missing or unexpected workload cell');
    let passes = 0;
    for (const key of cells) {
        const {runs, median_ns, vram_to_zram, meets_zram} = suite[key];
        if (!sameKeys(runs, backends) || !sameKeys(median_ns, backends)) throw Error('Invalid backend set in ' + key);
        for (const backend of backends) {
            const rows = runs[backend];
            if (!Array.isArray(rows) || rows.length !== 3 || !rows.every(({p50_ns, p99_ns}) =>
                Number.isSafeInteger(p50_ns * 2) && Number.isSafeInteger(p99_ns) && 0 < p50_ns && p50_ns <= p99_ns))
                throw Error('Invalid trial timing in ' + key);
            const middle = rows.map(r => r.p50_ns).sort((a, b) => a - b)[1];
            if (middle !== median_ns[backend]) throw Error('Median disagrees with trial evidence in ' + key);
        }
        const request = Buffer.alloc(49);
        request.write('Z');
        [...runs.vram, ...runs['zram-zstd']].forEach(({p50_ns}, i) =>
            request.writeBigUInt64BE(BigInt(p50_ns * 2), 1 + i * 8));
        const verdict = new TextDecoder().decode(await model.dispatch(request));
        if (!['yes', 'no'].includes(verdict) || (verdict === 'yes') !== meets_zram ||
            vram_to_zram !== median_ns.vram / median_ns['zram-zstd'])
            throw Error('Model verdict or ratio disagrees with timing evidence in ' + key);
        if (verdict === 'yes') passes++;
    }
    return {passes, total: cells.length};
}
const operator = await certify(receipt.workloads, ['patterned', 'dense']);
const mixed = await certify(receipt.mixed_overwrites, ['sparse', 'selector', 'period97', 'perturbed', 'independent', 'fill']);
const operatorParity = operator.passes === operator.total;
const mixedParity = mixed.passes === mixed.total;
if (receipt.operator_parity !== operatorParity || receipt.held_out_parity !== mixedParity ||
    receipt.meets_parity !== (operatorParity && mixedParity)) throw Error('Aggregate verdict disagrees with model');
receipt.prism_replay = {address, operator, mixed};
writeFileSync(output, JSON.stringify(receipt, null, 2) + '\n');
console.log(JSON.stringify(receipt.prism_replay));
if (gate && !receipt.meets_parity) process.exitCode = 1;
