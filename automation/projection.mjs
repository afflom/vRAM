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
const stats = {};
for (const route of routes) {
    const times = projection.trials.map((trial, index) => {
        const n = trial[route + '_ns'];
        if (trial.trial !== index || !Number.isSafeInteger(n) || n <= 0) throw Error('Invalid timed trial');
        return n;
    }).sort((a, b) => a - b);
    const total = times.reduce((a, b) => a + b, 0);
    if (!Number.isSafeInteger(total)) throw Error('Elapsed time exceeds exact integer precision');
    stats[route] = {total_ns: total, median_ns: (times[11] + times[12]) / 2, max_ns: times.at(-1)};
}
const {model, address} = await verifiedModel();
async function select(resident) {
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
    explicit_request: await select(false), resident_available: await select(true)};
writeFileSync(output, JSON.stringify(report, null, 2) + '\n');
console.log(JSON.stringify(report));
