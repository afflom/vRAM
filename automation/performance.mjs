// Evaluate equal-work guest trials against every observed latency threshold.
import {verifiedModel} from './verified-model.mjs';
import {createHash} from 'node:crypto';
import {readFileSync, writeFileSync} from 'node:fs';

const [input, output] = process.argv.slice(2);
if (!input || !output) throw Error('Usage: node automation/performance.mjs parallel-guest.json frontier.json');
const raw = readFileSync(input);
const receipt = JSON.parse(raw);
const {model, address} = await verifiedModel();

// The current parallel guest writes 16,384 pages and runs four trials per CPU count.
const rows = receipt.trials.filter(row => row.backend === 'vram');
const widths = [1, 2, 4, 8];
if (receipt.workload_mib !== 64 || rows.length !== widths.length * 4)
    throw Error('Expected the complete 64 MiB vRAM guest sweep');
async function decision(tag, values, bytes) {
    const request = Buffer.alloc(1 + bytes * values.length);
    request.write(tag);
    values.forEach((n, i) => {
        if (bytes === 4) request.writeUInt32BE(n, 1 + 4 * i);
        else request.writeBigUInt64BE(BigInt(n), 1 + 8 * i);
    });
    const answer = new TextDecoder().decode(await model.dispatch(request));
    if (answer !== 'yes' && answer !== 'no') throw Error('Unexpected performance-model response');
    return answer === 'yes';
}

const witnessFields = ['lane_write_ns', 'below_p99', 'at_p99'];
const hasWitnesses = receipt.timing_evidence_version === 1 ||
    rows.some(row => witnessFields.some(field => field in row));
if (receipt.timing_evidence_version !== undefined && receipt.timing_evidence_version !== 1)
    throw Error('Unknown timing evidence format');
const nonnegative = n => Number.isSafeInteger(n) && n >= 0;

async function certify(row) {
    const {below_p99: below, at_p99: equal, lane_write_ns: lanes} = row;
    const above = row.verified_pages - below - equal;
    if (![below, equal, above].every(nonnegative) || !Array.isArray(lanes) ||
        lanes.length !== row.vcpus || !lanes.every(nonnegative) ||
        !await decision('r', [below, equal, above], 4)) throw Error('Invalid guest rank or lane evidence');
    const [longest, other = 0] = [...lanes].sort((a, b) => b - a);
    const work = lanes.reduce((sum, n) => sum + BigInt(n), 0n);
    if (!longest || longest > row.write_ns || row.page_write_p99_ns > longest ||
        BigInt(row.page_write_p99_ns) * BigInt(equal + above) > work ||
        (row.page_write_p99_ns === 0 && below > 0)) throw Error('Inconsistent page and lane timings');
    const gain = longest - other;
    if (!await decision('k', [0, longest, other, gain], 8)) throw Error('Invalid single-lane gain bound');
    return {repeat: row.repeat, longest_lane_ns: longest, next_lane_ns: other,
        outside_longest_lane_ns: row.write_ns - longest, max_single_lane_gain_ns: gain};
}

const points = [];
for (const vcpus of widths) {
    const trials = rows.filter(row => row.vcpus === vcpus);
    if (trials.map(row => row.repeat).sort().join(',') !== '0,1,2,3')
        throw Error('Missing or duplicate repetition at ' + vcpus + ' vCPUs');
    const bounds = [];
    for (const row of trials) {
        if (row.verified_pages !== 16384 || !Number.isSafeInteger(row.write_ns) || row.write_ns <= 0 ||
            !Number.isSafeInteger(row.page_write_p99_ns) || row.page_write_p99_ns < 0 ||
            row.page_write_p99_ns > row.write_ns) throw Error('Invalid guest timing or readback');
        if (hasWitnesses) bounds.push(await certify(row));
    }
    const elapsed = trials.reduce((sum, row) => sum + row.write_ns, 0);
    if (!Number.isSafeInteger(elapsed)) throw Error('Aggregate time exceeds exact integer precision');
    points.push({vcpus, max_trial_p99_ns: Math.max(...trials.map(row => row.page_write_p99_ns)),
        total_write_ns: elapsed, throughput_mib_s: trials.length * receipt.workload_mib * 1e9 / elapsed,
        ...(hasWitnesses ? {lane_bounds: bounds} : {})});
}

// Once a width becomes eligible its time stays eligible at every looser budget.
// Finish every tie group before publishing that budget's fastest width.
const ordered = [...points].sort((a, b) => a.max_trial_p99_ns - b.max_trial_p99_ns);
const frontier = [];
let best = ordered[0];
for (const [index, point] of ordered.entries()) {
    const limit = point.max_trial_p99_ns;
    if (index && await decision('b', [limit, limit, point.total_write_ns, best.total_write_ns], 8))
        best = point;
    if (ordered[index + 1]?.max_trial_p99_ns !== limit && frontier.at(-1)?.vcpus !== best.vcpus)
        frontier.push({minimum_p99_budget_ns: limit, vcpus: best.vcpus});
}
const report = {input_sha256: createHash('sha256').update(raw).digest('hex'),
    model: address, accelerator: receipt.accelerator, host_cpus: receipt.host_cpus,
    guest_kernel: receipt.kernel, qemu: receipt.qemu, package_sha256: receipt.package_sha256,
    workload_mib_per_trial: receipt.workload_mib, trials_per_width: 4, rank_and_lane_witnesses: hasWitnesses, points, frontier};
writeFileSync(output, JSON.stringify(report, null, 2) + '\n');
console.log(JSON.stringify(report));
