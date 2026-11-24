// Replay the request and page identities before comparing hypothetical service savings.
import {createHash} from 'node:crypto';
import {gunzipSync} from 'node:zlib';

const mixed = ['sparse', 'selector', 'period97', 'perturbed', 'independent', 'fill'];
const profiles = ['patterned', 'dense', ...mixed];
const integer = n => Number.isSafeInteger(n) && n >= 0;

function median(values) {
    const sorted = [...values].sort((a, b) => a - b);
    const half = Math.floor(sorted.length / 2);
    return sorted.length % 2 ? sorted[half] : (sorted[half - 1] + sorted[half]) / 2;
}

function expectedPage(page, profile, run) {
    if (mixed.includes(profile))
        return page >= 14000 && page < 14384 && mixed[(page - 14000 + run) % mixed.length] === profile;
    const start = 256 + 2048 * (run * 2 + profiles.indexOf(profile));
    return start <= page && page < start + 2048;
}

export async function replayPath(receipt, compressed, parity) {
    if (createHash('sha256').update(compressed).digest('hex') !== receipt.trace_sha256)
        throw Error('Store clock evidence digest mismatch');
    const trials = new Map();
    const ordered = [];
    for (const [run, profile, op, batch] of JSON.parse(gunzipSync(compressed))) {
        if (!integer(run) || run >= 3 || !profiles.includes(profile) || !['read', 'write'].includes(op))
            throw Error('Unknown store clock trial');
        const key = [profile, op, run].join('/');
        if (!trials.has(key)) trials.set(key, {pages: new Set(), times: []});
        const {pages, times} = trials.get(key);
        for (const row of batch) {
            const [seq, page, start, enter, leave, finish] = row;
            if (row.length !== 6 || !row.every(integer) || !seq || !expectedPage(page, profile, run) ||
                pages.has(page) || !(start <= enter && enter <= leave && leave <= finish && start < finish))
                throw Error('Invalid page identity or nested interval in ' + key);
            pages.add(page);
            ordered.push([seq, start, finish]);
            times.push({total: finish - start, before: enter - start, store: leave - enter,
                after: finish - leave, outside: (enter - start) + (finish - leave)});
        }
    }
    ordered.sort((a, b) => a[0] - b[0]);
    for (let i = 1; i < ordered.length; i++)
        if (ordered[i][0] !== ordered[i - 1][0] + 1 || ordered[i][1] < ordered[i - 1][2])
            throw Error('Missing, duplicated or overlapping store clock request');

    const cells = {};
    let outsidePasses = 0;
    let servicePasses = 0;
    for (const profile of profiles) for (const op of ['write', 'read']) {
        const key = profile + '_' + op;
        const reference = (mixed.includes(profile) ? receipt.mixed_overwrites : receipt.workloads)[key];
        const runs = [];
        for (let run = 0; run < 3; run++) {
            const times = trials.get([profile, op, run].join('/'))?.times;
            if (times?.length !== (mixed.includes(profile) ? 64 : 2048))
                throw Error('Incomplete store clock coverage in ' + key);
            const fields = Object.keys(times[0]);
            const medians = Object.fromEntries(fields.map(field => [field, median(times.map(t => t[field]))]));
            const observed = reference.runs.control[run];
            const totals = times.map(t => t.total).sort((a, b) => a - b);
            if (medians.total !== observed.p50_ns || totals[Math.ceil(0.99 * totals.length) - 1] !== observed.p99_ns)
                throw Error('Store clock and syscall latencies disagree in ' + key);
            runs.push(medians);
        }
        const outside = runs.map(t => t.outside);
        const budgets = reference.runs['zram-zstd'].map(t => t.p50_ns);
        const canMeet = await parity(outside, budgets);
        const serviceOnly = await parity(runs.map(t => t.store), budgets);
        if (canMeet) outsidePasses++;
        if (serviceOnly) servicePasses++;
        cells[key] = {runs_p50_ns: runs, median_ns: Object.fromEntries(Object.keys(runs[0])
            .map(k => [k, median(runs.map(t => t[k]))])), zram_p50_ns: median(budgets),
            zero_store_meets_zram: canMeet, zero_outside_meets_zram: serviceOnly,
            outside_excess_ns: Math.max(0, median(outside) - median(budgets))};
    }
    return {traced_backend: 'control', clock: 'CLOCK_MONOTONIC', requests: ordered.length,
        cells, zero_store_passes: outsidePasses, zero_outside_passes: servicePasses};
}
