// Replay the median-of-three contract on each operator and mixed-overwrite cell.
import {readFileSync, writeFileSync} from 'node:fs';
import {replayPath} from './swap-path.mjs';
import {verifiedModel} from './verified-model.mjs';

const [input, output, ...options] = process.argv.slice(2);
const gate = options[0] === '--require-parity';
if (gate) options.shift();
const completion = options[0] === '--completion';
if (completion) options.shift();
const windows = options[0] === '--read-windows';
if (windows) options.shift();
const literal = options[0] === '--kernel-literal';
if (literal) options.shift();
const [traceOption, traceFile] = options;
if (!input || !output || (options.length && (options.length !== 2 || traceOption !== '--trace')))
    throw Error('Usage: node automation/swap-latency.mjs input.json output.json [--require-parity] [--completion] [--read-windows] [--kernel-literal] [--trace samples.json.gz]');
const receipt = JSON.parse(readFileSync(input));
const {model, address} = await verifiedModel();
const candidate = literal ? 'kernel-literal' : 'vram';
const control = literal ? 'vram-nbd' : 'control';
const backends = [control, candidate, 'zram-zstd'];
const sameKeys = (obj, keys) => JSON.stringify(Object.keys(obj).sort()) === JSON.stringify([...keys].sort());
if (literal !== Object.hasOwn(receipt, 'literal_policy') || (literal && (
    receipt.literal_policy !== address || receipt.literal_payload_bytes !== 64 * 1024 * 1024 ||
    !sameKeys(receipt.server_sha256, [control, candidate]) ||
    !Object.values(receipt.server_sha256).every(hash => /^[a-f0-9]{64}$/.test(hash)) ||
    completion || windows || traceFile || receipt.rq_affinity || receipt.read_windows)))
    throw Error('Invalid kernel literal comparison or policy identity');
if (receipt.page_bytes !== 4096 || receipt.queue_depth !== 1 || receipt.runs !== 3 ||
    receipt.samples_per_run !== 2048 || receipt.verified_bytes_per_backend !== 3 * 2 * 2048 * 4096 ||
    receipt.mixed_pages_per_family_per_round !== 64 || receipt.mixed_verified_bytes_per_backend !== 3 * 6 * 64 * 4096)
    throw Error('Invalid workload dimensions or readback count');

async function parity(candidate, reference) {
    const request = Buffer.alloc(49);
    request.write('Z');
    [...candidate, ...reference].forEach((ns, i) => request.writeBigUInt64BE(BigInt(ns * 2), 1 + i * 8));
    const verdict = new TextDecoder().decode(await model.dispatch(request));
    if (!['yes', 'no'].includes(verdict)) throw Error('Invalid Prism parity reply');
    return verdict === 'yes';
}

async function certify(suite, profiles, operations = ['write', 'read']) {
    const cells = profiles.flatMap(profile => operations.map(op => profile + '_' + op));
    if (!sameKeys(suite, cells)) throw Error('Missing or unexpected workload cell');
    let passes = 0;
    for (const key of cells) {
        const {runs, median_ns, meets_zram} = suite[key];
        if (!sameKeys(runs, backends) || !sameKeys(median_ns, backends)) throw Error('Invalid backend set in ' + key);
        for (const backend of backends) {
            const rows = runs[backend];
            if (!Array.isArray(rows) || rows.length !== 3 || !rows.every(({p50_ns, p99_ns}) =>
                Number.isSafeInteger(p50_ns * 2) && Number.isSafeInteger(p99_ns) && 0 < p50_ns && p50_ns <= p99_ns))
                throw Error('Invalid trial timing in ' + key);
            const middle = rows.map(r => r.p50_ns).sort((a, b) => a - b)[1];
            if (middle !== median_ns[backend]) throw Error('Median disagrees with trial evidence in ' + key);
        }
        const meets = await parity(runs[candidate].map(r => r.p50_ns), runs['zram-zstd'].map(r => r.p50_ns));
        if (meets !== meets_zram ||
            suite[key][literal ? 'literal_to_zram' : 'vram_to_zram'] !== median_ns[candidate] / median_ns['zram-zstd'])
            throw Error('Model verdict or ratio disagrees with timing evidence in ' + key);
        if (meets) passes++;
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
if (literal) {
    receipt.literal_at_most_nbd_p50 = {};
    for (const [key, {runs}] of Object.entries({...receipt.workloads, ...receipt.mixed_overwrites})) {
        receipt.literal_at_most_nbd_p50[key] = await parity(runs[candidate].map(t => t.p50_ns), runs[control].map(t => t.p50_ns));
    }
}
if (windows && !receipt.read_windows) throw Error('Missing direct read window evidence');
if (receipt.read_windows) {
    const sizes = [1, 2, 16, 64, 65, 128];
    if (receipt.read_window_samples_per_run !== 128 ||
        receipt.read_window_verified_bytes_per_backend !== sizes.reduce((a,b) => a+b) * 4096 * 128 * 3 ||
        receipt.rq_affinity || traceFile) throw Error('Invalid direct read window dimensions');
    await certify(receipt.read_windows, sizes.map(n => 'pages' + n), ['read']);
    const comparison = {};
    for (const [key, {runs}] of Object.entries(receipt.read_windows)) {
        comparison[key] = await parity(runs.vram.map(t => t.p50_ns), runs.control.map(t => t.p50_ns));
    }
    receipt.read_window_at_most_control_p50 = comparison;
}
if (Boolean(receipt.trace_sha256) !== Boolean(traceFile) || (receipt.store_path && !traceFile)) throw Error('Store clock receipt requires matching trace');
if (completion && !receipt.rq_affinity) throw Error('Missing completion placement evidence');
if (receipt.rq_affinity) {
    const {control, vram} = receipt.rq_affinity;
    if (!sameKeys(receipt.rq_affinity, ['control', 'vram']) || ![0, 1, 2].includes(control) ||
        vram !== 0 || traceFile || !/^[a-f0-9]{64}$/.test(receipt.server_sha256?.control ?? '') ||
        receipt.server_sha256.control !== receipt.server_sha256.candidate)
        throw Error('Invalid completion placement control');
    const cells = {};
    for (const [key, {runs}] of Object.entries({...receipt.workloads, ...receipt.mixed_overwrites})) {
        cells[key] = await parity(runs.vram.map(t => t.p50_ns), runs.control.map(t => t.p50_ns));
    }
    receipt.completion_comparison = {distinct_settings: control !== vram, at_most_default_p50: cells};
}
if (traceFile) receipt.store_path = await replayPath(receipt, readFileSync(traceFile), parity);
writeFileSync(output, JSON.stringify(receipt, null, 2) + '\n');
console.log(JSON.stringify(receipt.prism_replay));
if (gate && !receipt.meets_parity) process.exitCode = 1;
