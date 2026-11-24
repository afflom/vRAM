import {execFileSync} from 'node:child_process';
import {join} from 'node:path';
import {fileURLToPath} from 'node:url';
import {load} from 'prism-code';

export async function verifiedModel() {
    const root = fileURLToPath(new URL('../', import.meta.url));
    const result = JSON.parse(execFileSync(join(root, 'automation/node_modules/.bin/prism-code'),
        ['verify', '.', '--json'], {cwd: root, encoding: 'utf8'}));
    if (!result.ok || !result.summary.identitiesVerified) throw Error('Performance model did not verify');
    const model = await load(join(root, result.summary.artifact));
    if (!model.identities.verified) throw Error('Unverified model artifact');
    return {model, address: result.summary.address};
}
