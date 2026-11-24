// Specialize the verified Holo controller over the complete Linux adapter input domain.
import {createHash} from 'node:crypto';
import {readFileSync, writeFileSync} from 'node:fs';
import {verifiedModel} from './verified-model.mjs';

const [header] = process.argv.slice(2);
if (!header) throw Error('Expected output header path');
const source = path => readFileSync(new URL(path, import.meta.url));
const imported = source('../app/src/Main.lex.tex');
const manifest = JSON.parse(source('../device/proof/runtime/build-manifest.json'));
const runtime = manifest.inputs.find(input => input.kind === 'source' && input.path === 'src/Main.lex.tex');
if (!imported.equals(source('../device/proof/runtime/project/src/Main.lex.tex')) ||
    createHash('sha256').update(imported).digest('hex') !== runtime?.sha256)
    throw Error('Imported Verdant representation chooser differs from certified source');
const {model, address} = await verifiedModel();
async function actions(tag) {
    const table = [];
    for (let op = 0; op < 3; op++) {
        for (let shape = 0; shape < 8; shape++) {
            const packet = Uint8Array.of(tag, op, shape >> 2, (shape >> 1) & 1, shape & 1);
            const action = new TextDecoder().decode(await model.dispatch(packet));
            if (!/^[012]$/.test(action)) throw Error('Invalid transfer policy response');
            table.push(Number(action));
        }
    }
    return table.join(', ');
}
const fills = [];
for (const uniform of [0, 1]) {
    const choice = new TextDecoder().decode(await model.dispatch(Uint8Array.of(77, uniform)));
    if (!/^[01]$/.test(choice)) throw Error('Invalid Verdant representation');
    fills.push(Number(choice));
}
async function layout(field) {
    const value = new TextDecoder().decode(await model.dispatch(Uint8Array.of(79, field)));
    if (!/^\d{5}$/.test(value)) throw Error('Invalid packed directory constant');
    return Number(value);
}
writeFileSync(header, `/* Generated from the verified Prism model. */\n#define VRAM_HOLO "${address}"\n#define VRAM_SLOT_LOCK ${await layout(0)}UL\n#define VRAM_SLOT_BYTES ${await layout(1)}\nstatic const u8 inline_fill[2] = {${fills.join(', ')}};\nstatic const u8 resident_action[24] = {${await actions(78)}};\nstatic const u8 literal_action[24] = {${await actions(76)}};\n`);
