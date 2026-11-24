// Read the release exactly as an offline consumer does and compare its wire responses.
import {readFile} from 'node:fs/promises';
import {load} from 'prism-code';

const [archive, catalog] = process.argv.slice(2);
const model = await load(archive);
if (!model.identities.verified || !model.world.pure) throw Error('Expected a verified, pure Holo');
const {facets} = JSON.parse(await readFile(catalog, 'utf8'));

async function requireReply(request, expected) {
    const actual = await model.dispatch(Buffer.from(request));
    if (!Buffer.from(actual).equals(Buffer.from(expected))) {
        throw Error('Facet reply differs from the catalog for ' + request);
    }
}
await requireReply('F/', JSON.stringify(facets.map(group => group.id)));
for (const group of facets) await requireReply('F/' + group.id, JSON.stringify(group));
await requireReply('F/missing', 'invalid request');
console.log('Verified archived facet records:', facets.reduce((sum, group) => sum + group.records.length, 0));
