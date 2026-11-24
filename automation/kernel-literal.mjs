// Specialize the verified Holo controller over the complete Linux adapter input domain.
import {writeFileSync} from 'node:fs';
import {verifiedModel} from './verified-model.mjs';

const [header] = process.argv.slice(2);
if (!header) throw Error('Expected output header path');
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
writeFileSync(header, `/* Generated from the verified Prism model. */\n#define VRAM_HOLO "${address}"\nstatic const u8 literal_action[24] = {${actions.join(', ')}};\n`);
