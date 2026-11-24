// Exercise the deployed file layout and the real Prism browser runtime over HTTP.
import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {mkdtemp, readFile, rm} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import {setTimeout as delay} from 'node:timers/promises';

const base = new URL(process.argv[2]);
const profile = await mkdtemp(join(tmpdir(), 'vram-browser-'));
const browser = spawn(process.env.CHROME_BIN || 'google-chrome', ['--headless', '--no-sandbox',
  '--disable-dev-shm-usage', '--disable-gpu', '--remote-debugging-port=0',
  '--user-data-dir=' + profile, 'about:blank'], {stdio: 'ignore'});
const exited = new Promise(resolve => browser.once('exit', resolve));
async function until(check, description) {
  for (let i = 0; i < 200; i++) {
    const value = await check();
    if (value) return value;
    await delay(100);
  }
  throw Error('Timed out waiting for ' + description);
}
try {
  const port = await until(async () => {
    try { return (await readFile(join(profile, 'DevToolsActivePort'), 'utf8')).split('\n')[0]; }
    catch { return false; }
  }, 'Chromium');
  const tabs = await (await fetch('http://127.0.0.1:' + port + '/json/list')).json();
  const socket = new WebSocket(tabs.find(t => t.type === 'page').webSocketDebuggerUrl);
  await new Promise((resolve, reject) => { socket.onopen = resolve; socket.onerror = reject; });
  const pending = new Map();
  let id = 0;
  socket.onmessage = ({data}) => {
    const message = JSON.parse(data);
    if (pending.has(message.id)) {
      pending.get(message.id)(message);
      pending.delete(message.id);
    }
  };
  async function call(method, params) {
    const request = ++id;
    const response = new Promise(resolve => pending.set(request, resolve));
    socket.send(JSON.stringify({id: request, method, params}));
    const message = await response;
    if (message.error) throw Error(JSON.stringify(message.error));
    return message.result;
  }
  async function evaluate(expression) {
    const value = await call('Runtime.evaluate', {expression, returnByValue: true, awaitPromise: true});
    if (value.exceptionDetails) throw Error(value.exceptionDetails.exception?.description || value.exceptionDetails.text);
    return value.result.value;
  }
  async function navigate(path) {
    const url = new URL(path, base).href;
    await call('Page.navigate', {url});
    await until(() => evaluate('location.href === ' + JSON.stringify(url) + ' && document.readyState === "complete"'), url);
  }
  await navigate('./');
  assert.equal(await evaluate('document.querySelector(".hero-art img").naturalWidth'), 1536);
  await evaluate('theme.value="dark"; theme.dispatchEvent(new Event("change"))');
  await call('Emulation.setDeviceMetricsOverride', {width: 375, height: 812, deviceScaleFactor: 1, mobile: true});
  await call('Network.enable', {});
  await call('Network.setBlockedURLs', {urls: ['*facets.json']});
  for (const path of ['./', 'facets.html', 'runtime/']) {
    await navigate(path);
    assert.equal(await evaluate('document.documentElement.dataset.theme'), 'dark');
    assert.ok(await evaluate('document.documentElement.scrollWidth <= 375'), path + ' overflows on mobile');
    if (path === 'facets.html') {
      assert.equal(await evaluate('document.querySelectorAll(".ref-group").length'), 10);
      assert.equal(await evaluate('document.querySelectorAll(".design-view img").length'), 4);
    }
  }
  await until(() => evaluate('!document.querySelector("#run").disabled'), 'verified model startup');
  assert.equal(await evaluate('answer.textContent'), 'advance');
  await evaluate('checkpoint.elements.ceiling.click(); document.querySelector("#run").click()');
  await until(() => evaluate('answer.textContent === "complete"'), 'ceiling decision');
  await evaluate('checkpoint.elements.verified.click(); document.querySelector("#run").click()');
  await until(() => evaluate('answer.textContent === "failed readback"'), 'readback decision');
  await until(() => evaluate('document.getElementById("facet-group").options.length === 10'), 'archived facet index');
  await evaluate(`const group = document.getElementById('facet-group');
    group.value = 'security'; group.dispatchEvent(new Event('change'))`);
  await until(() => evaluate('document.getElementById("facet-record").value === "Product.Security::assetPages"'), 'archived security records');
  assert.ok(await evaluate('document.getElementById("facet-fields").textContent.includes("componentIndex")'));
  await evaluate('replay.click()');
  await until(() => evaluate('acceptance.textContent === "111 of 111 acceptance vectors passed."'), 'acceptance replay');
  console.log(await evaluate('acceptance.textContent'));
  socket.close();
} finally {
  browser.kill();
  await exited;
  await rm(profile, {recursive: true, force: true, maxRetries: 5});
}
