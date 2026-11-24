const form = document.querySelector('#checkpoint');
const answer = document.querySelector('#answer');
const replay = document.querySelector('#replay');
const acceptance = document.querySelector('#acceptance');

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
} catch {
    answer.textContent = 'The verified model could not be loaded. Reload this page to retry.';
}
