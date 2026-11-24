// Evaluate equal-work guest trials against every observed latency threshold.
import {execFileSync} from 'node:child_process';
import {createHash} from 'node:crypto';
import {readFileSync, writeFileSync} from 'node:fs';
import {join} from 'node:path';
import {fileURLToPath} from 'node:url';
import {load} from 'prism-code';

const [input, output] = process.argv.slice(2);
if (!input || !output) throw Error('Usage: node automation/performance.mjs parallel-guest.json frontier.json');
const raw = readFileSync(input);
const receipt = JSON.parse(raw);
const root = fileURLToPath(new URL('../', import.meta.url));
const result = JSON.parse(execFileSync(join(root, 'automation/node_modules/.bin/prism-code'),
    ['verify', '.', '--json'], {cwd: root, encoding: 'utf8'}));
if (!result.ok || !result.summary.identitiesVerified) throw Error('Performance model did not verify');
const model = await load(join(root, result.summary.artifact));
if (!model.identities.verified) throw Error('Unverified model artifact');

// The current parallel guest writes 16,384 pages and runs four trials per CPU count.
const rows = receipt.trials.filter(row => row.backend === 'vram');
const widths = [1, 2, 4, 8];
if (receipt.workload_mib !== 64 || rows.length !== widths.length * 4)
    throw Error('Expected the complete 64 MiB vRAM guest sweep');
const points = widths.map(vcpus => {
    const trials = rows.filter(row => row.vcpus === vcpus);
    if (trials.map(row => row.repeat).sort().join(',') !== '0,1,2,3')
        throw Error('Missing or duplicate repetition at ' + vcpus + ' vCPUs');
    for (const row of trials) {
        if (row.verified_pages !== 16384 || !Number.isSafeInteger(row.write_ns) || row.write_ns <= 0 ||
            !Number.isSafeInteger(row.page_write_p99_ns) || row.page_write_p99_ns < 0 ||
            row.page_write_p99_ns > row.write_ns) throw Error('Invalid guest timing or readback');
    }
    const elapsed = trials.reduce((sum, row) => sum + row.write_ns, 0);
    if (!Number.isSafeInteger(elapsed)) throw Error('Aggregate time exceeds exact integer precision');
    return {vcpus, max_trial_p99_ns: Math.max(...trials.map(row => row.page_write_p99_ns)),
        total_write_ns: elapsed, throughput_mib_s: trials.length * receipt.workload_mib * 1e9 / elapsed};
});

async function improves(limit, point, incumbent) {
    const request = Buffer.alloc(33);
    request.write('b');
    [limit, point.max_trial_p99_ns, point.total_write_ns, incumbent.total_write_ns]
        .forEach((n, i) => request.writeBigUInt64BE(BigInt(n), 1 + 8 * i));
    const answer = new TextDecoder().decode(await model.dispatch(request));
    if (answer !== 'yes' && answer !== 'no') throw Error('Unexpected performance-model response');
    return answer === 'yes';
}

const deadlines = [...new Set(points.map(p => p.max_trial_p99_ns))].sort((a, b) => a - b);
let best = points.find(p => p.max_trial_p99_ns === deadlines[0]);
const frontier = [];
for (const limit of deadlines) {
    for (const point of points) {
        if (await improves(limit, point, best)) best = point;
    }
    if (frontier.at(-1)?.vcpus !== best.vcpus)
        frontier.push({minimum_p99_budget_ns: limit, vcpus: best.vcpus});
}
const report = {input_sha256: createHash('sha256').update(raw).digest('hex'),
    model: result.summary.address, accelerator: receipt.accelerator, host_cpus: receipt.host_cpus,
    guest_kernel: receipt.kernel, qemu: receipt.qemu, package_sha256: receipt.package_sha256,
    workload_mib_per_trial: receipt.workload_mib, trials_per_width: 4, points, frontier};
writeFileSync(output, JSON.stringify(report, null, 2) + '\n');
console.log(JSON.stringify(report));
