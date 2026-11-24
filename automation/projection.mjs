// Compare equal verified guest projections with and without a resident operand.
import {createHash} from 'node:crypto';
import {readFileSync, writeFileSync} from 'node:fs';
import {verifiedModel} from './verified-model.mjs';

const [input, output] = process.argv.slice(2);
if (!input || !output) throw Error('Usage: node automation/projection.mjs linux-dimm.json routes.json');
const raw = readFileSync(input);
const receipt = JSON.parse(raw);
const {projection} = receipt;
if (projection?.port !== 'org.verdant.project.1' || projection.columns !== 64 ||
    projection.verified_trials !== 24 || projection.trials.length !== 24)
    throw Error('Expected 24 verified 64-column projections');
const routes = ['packed', 'restore_cpu', 'resident_cpu'];
function summarize(trials, names, rows) {
    const stats = {};
    for (const route of names) {
        const times = trials.map((trial, index) => {
            const n = trial[route + '_ns'];
            if (trial.trial !== index || !Number.isSafeInteger(n) || n <= 0) throw Error('Invalid timed trial');
            return n;
        }).sort((a, b) => a - b);
        const total = times.reduce((a, b) => a + b, 0);
        if (!Number.isSafeInteger(total)) throw Error('Elapsed time exceeds exact integer precision');
        const mid = times.length / 2;
        stats[route] = {total_ns: total, median_ns: (times[mid - 1] + times[mid]) / 2,
            max_ns: times.at(-1), rows_per_s: trials.length * rows * 1e9 / total};
    }
    return stats;
}
const stats = summarize(projection.trials, routes, 1);
const {model, address} = await verifiedModel();
async function select(stats, resident) {
    const q = Buffer.alloc(26);
    q.write(resident ? 'j1' : 'j0');
    routes.forEach((name, i) => q.writeBigUInt64BE(BigInt(stats[name].total_ns), 2 + 8*i));
    const answer = new TextDecoder().decode(await model.dispatch(q));
    if (!['packed', 'restore', ...(resident ? ['resident'] : [])].includes(answer))
        throw Error('Unexpected projection route');
    return answer;
}
const report = {input_sha256: createHash('sha256').update(raw).digest('hex'), model: address,
    package_sha256: receipt.package_sha256, accelerator: receipt.acceleration, stats,
    explicit_request: await select(stats, false), resident_available: await select(stats, true)};
if (projection.batch_version !== undefined || projection.batch_trials !== undefined) {
    const widths = [2, 5, 6, 8, 31, 32, 64];
    if (projection.batch_version !== 1 || projection.batch_trials?.length !== 8 * widths.length)
        throw Error('Incomplete batch sweep');
    report.batches = [];
    for (const rows of widths) {
        const trials = projection.batch_trials.filter(t => t.rows === rows);
        if (trials.length !== 8) throw Error('Incomplete width: ' + rows);
        const group = summarize(trials, [...routes, 'scalar'], rows);
        const tiles = Math.ceil(rows / 31);
        const q = Buffer.alloc(9);
        q.write('w');
        q.writeUInt32BE(rows, 1);
        q.writeUInt32BE(tiles, 5);
        const verdict = new TextDecoder().decode(await model.dispatch(q));
        if (!['yes', 'no'].includes(verdict)) throw Error('Invalid batch traffic witness');
        // VR-29 compares a direct, restore-once, and resident route. Keep the
        // repeated scalar measurements separately as the pre-batching control.
        report.batches.push({rows, tiles, stats: group, batch_smaller_than_restore: verdict === 'yes',
            explicit_request: await select(group, false), resident_available: await select(group, true)});
    }
}
writeFileSync(output, JSON.stringify(report, null, 2) + '\n');
console.log(JSON.stringify(report));
