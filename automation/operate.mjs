// Studio's measured next/advance loop applied to the full Verdant page store.
import {spawnSync} from 'node:child_process';
import {createHash} from 'node:crypto';
import {readFileSync, writeFileSync, renameSync, existsSync, mkdirSync, mkdtempSync, rmSync} from 'node:fs';
import {cpus, release, arch} from 'node:os';
import {resolve, dirname, join} from 'node:path';
import {fileURLToPath} from 'node:url';
import {load} from 'prism-code';

const root = fileURLToPath(new URL('../', import.meta.url));
const [profile, ceilingText, budgetText, filename] = process.argv.slice(2);
const ceiling = Number(ceilingText), budget = Number(budgetText);
if (!/^[SFUM]$/.test(profile ?? '') || !Number.isSafeInteger(ceiling) || ceiling < 4 ||
    ceiling > 65536 || !Number.isSafeInteger(budget) || budget < 8 || budget > 65536 || !filename) {
  throw Error('Usage: node automation/operate.mjs S|F|U|M CEILING_MiB(4..65536) RSS_BUDGET_MiB(8..65536) OUTPUT.json');
}
const out = resolve(filename);
if (existsSync(out)) throw Error('Use a fresh output path for each experiment');
mkdirSync(dirname(out), {recursive: true});
const binary = join(root, 'host/build/vram-scale');
const prism = join(root, 'automation/node_modules/.bin/prism-code');
const digest = path => createHash('sha256').update(readFileSync(path)).digest('hex');
function execute(command, args) {
  const p = spawnSync(command, args, {cwd: root, encoding: 'utf8', env: {...process.env, PRISM_CODE_TIER: 'offline'}});
  if (p.error || p.status !== 0) throw Error([command, p.error, p.stderr, p.stdout].join(' '));
  return p.stdout;
}
const verified = JSON.parse(execute(prism, ['verify', '.', '--json']));
if (!verified.ok || !verified.summary.identitiesVerified || !verified.summary.vectorsTotal ||
    verified.summary.vectorsPassed !== verified.summary.vectorsTotal) throw Error('Project verification failed');
const holo = join(root, verified.summary.artifact);
const model = await load(holo);
if (!model.identities.verified) throw Error('Model identity failed');
async function dispatch(bytes) {
  return new TextDecoder().decode(await model.dispatch(bytes));
}
function packet(tag, numbers) {
  const bytes = Buffer.alloc(tag.length + 4 * numbers.length);
  bytes.write(tag);
  numbers.forEach((value, index) => {
    if (!Number.isSafeInteger(value) || value < 0) throw Error('Invalid measurement');
    bytes.writeUInt32BE(value, tag.length + index * 4);
  });
  return bytes;
}
function assess(row, limit) {
  return dispatch(packet('v', [row.stored_pages, row.verified_pages, row.requested_mib,
    row.peak_rss_kib, budget * 1024, limit]));
}
async function nextExtent(previous) {
  const response = await dispatch(packet('g', [previous, ceiling]));
  const next = Number(response);
  if (!/^\d{5}$/.test(response) || next <= previous || next > ceiling) throw Error('Invalid extent from model');
  return next;
}
function trial(mib, nodes) {
  return JSON.parse(execute(binary, [String(mib), String(budget), profile, String(nodes)]));
}
const receipt = {backend: 'native Verdant store', profile, ceiling_mib: ceiling, rss_budget_mib: budget,
  cpu: cpus()[0].model, kernel: release(), arch: arch(), model: verified.summary.address,
  runner_sha256: digest(binary), operator_sha256: digest(fileURLToPath(import.meta.url)), pilots: [], checkpoints: []};
const working = mkdtempSync(join(dirname(out), '.vram-'));
function save() {
  const pending = join(working, 'receipt');
  writeFileSync(pending, JSON.stringify(receipt, null, 2) + '\n');
  renameSync(pending, out);
}
try {
  // Calibrate on the same page distribution. Ordering is decided inside the
  // holo from capacity and time observations for candidate and incumbent.
  let best;
  const first = await nextExtent(0);
  for (let index = 0;; index++) {
    const setting = await dispatch(Buffer.from('q' + index));
    if (setting === 'done') break;
    if (!/^\d+$/.test(setting) || Number(setting) > 49) throw Error('Invalid pilot from model');
    const sample = trial(first, Number(setting));
    const admission = await assess(sample, 0xffffffff);
    if (admission !== 'advance') {
      receipt.pilots.push({...sample, decision: admission});
      save();
      if (admission === 'budget') continue;
      throw Error('Pilot stopped: ' + admission);
    }
    const decision = await dispatch(packet('p1', [sample.store_bytes, best?.store_bytes ?? 0xffffffff,
      Math.round(sample.write_seconds * 1e6), best ? Math.round(best.write_seconds * 1e6) : 0xffffffff]));
    receipt.pilots.push({...sample, decision});
    save();
    if (decision === 'replace') best = sample;
    else if (decision !== 'keep') throw Error('Pilot stopped: ' + decision);
  }
  if (!best) {
    receipt.outcome = 'budget';
    save();
    console.log(profile, 'all pilots exceeded the RSS budget');
  } else {
    receipt.search_nodes = best.search_nodes;
    for (let mib = first;; mib = await nextExtent(mib)) {
      const row = trial(mib, best.search_nodes);
      const decision = await assess(row, ceiling);
      receipt.checkpoints.push({...row, decision});
      save();
      console.log(profile, mib, decision, row.verified_pages, row.peak_rss_kib);
      if (decision === 'budget' || decision === 'complete') {
        receipt.outcome = decision;
        save();
        break;
      }
      if (decision !== 'advance') throw Error('Capacity sweep stopped: ' + decision);
    }
  }
} finally {
  rmSync(working, {recursive: true, force: true});
}
