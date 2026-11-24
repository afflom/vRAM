const form = document.querySelector('#checkpoint');
const answer = document.querySelector('#answer');
const replay = document.querySelector('#replay');
const acceptance = document.querySelector('#acceptance');

async function inspectFacets(model) {
    const groups = document.querySelector('#facet-group');
    const records = document.querySelector('#facet-record');
    const status = document.querySelector('#facet-status');
    const fields = document.querySelector('#facet-fields');
    let current = [];
    let generation = 0;
    const read = async slug => JSON.parse(new TextDecoder().decode(
        await model.dispatch(new TextEncoder().encode('F/' + slug))));
    function showRecord() {
        fields.replaceChildren();
        const record = current[records.selectedIndex];
        if (!record) return;
        for (const [name, value] of Object.entries(record.fields)) {
            const term = document.createElement('dt');
            term.textContent = name;
            const detail = document.createElement('dd');
            detail.textContent = typeof value === 'string' ? value : JSON.stringify(value, null, 2);
            fields.append(term, detail);
        }
    }
    async function showGroup() {
        const selected = ++generation;
        records.disabled = true;
        records.replaceChildren();
        fields.replaceChildren();
        current = [];
        status.textContent = 'Reading from the archive…';
        try {
            const group = await read(groups.value);
            if (selected !== generation) return;
            current = group.records;
            for (const record of current) records.add(new Option(record.id, record.id));
            records.disabled = !current.length;
            status.textContent = group.title + ' · ' + current.length + ' records';
            showRecord();
        } catch {
            if (selected === generation) status.textContent = 'Could not read this facet from the archive.';
        }
    }
    const index = await read('');
    for (const id of index) groups.add(new Option(id, id));
    groups.disabled = false;
    groups.addEventListener('change', showGroup);
    records.addEventListener('change', showRecord);
    await showGroup();
}

try {
    const {load} = await import('./vendor/src/browser.mjs');
    const model = await load(new URL('./app.holo', import.meta.url));
    if (!model.identities.verified || !model.world.pure) throw Error('Unexpected model identity or permissions');
    const evaluate = async () => {
        const flags = ['verified', 'fits', 'ceiling'].map(name => form.elements[name].checked ? '1' : '0');
        const bytes = await model.dispatch(new TextEncoder().encode(flags.join('')));
        answer.textContent = new TextDecoder().decode(bytes);
    };
    form.addEventListener('submit', async event => {
        event.preventDefault();
        try { await evaluate(); }
        catch { answer.textContent = 'The checkpoint could not be evaluated.'; }
    });
    replay.addEventListener('click', async () => {
        replay.disabled = true;
        acceptance.textContent = 'Checking vectors…';
        try {
            const {passed, total} = await model.acceptance();
            acceptance.textContent = `${passed} of ${total} acceptance vectors passed.`;
        } catch { acceptance.textContent = 'The acceptance run failed.'; }
        finally { replay.disabled = false; }
    });
    await evaluate();
    document.querySelector('#run').disabled = replay.disabled = false;
    inspectFacets(model).catch(() => {
        document.querySelector('#facet-status').textContent = 'Could not read the facet index from the archive.';
    });
} catch {
    answer.textContent = 'The verified model could not be loaded. Reload this page to retry.';
}
