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
const actions = [];
for (let op = 0; op < 3; op++) {
    for (let shape = 0; shape < 8; shape++) {
        const packet = Uint8Array.of(76, op, shape >> 2, (shape >> 1) & 1, shape & 1);
        const action = new TextDecoder().decode(await model.dispatch(packet));
        if (!/^[012]$/.test(action)) throw Error('Invalid literal policy response');
        actions.push(Number(action));
    }
}
const fills = [];
for (const uniform of [0, 1]) {
    const choice = new TextDecoder().decode(await model.dispatch(Uint8Array.of(77, uniform)));
    if (!/^[01]$/.test(choice)) throw Error('Invalid Verdant representation');
    fills.push(Number(choice));
}
writeFileSync(header, `/* Generated from the verified Prism model. */\n#define VRAM_HOLO "${address}"\nstatic const u8 inline_fill[2] = {${fills.join(', ')}};\nstatic const u8 literal_action[24] = {${actions.join(', ')}};\n`);
