const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const html = fs.readFileSync(path.join(__dirname, '../main/maintenance.html'), 'utf8');
const script = html.match(/<script>([\s\S]*?)<\/script>/)[1];

async function page(fetchAction, confirmation=true, resetPending=false) {
  const elements = Object.fromEntries(['#exit', '#upload', '#reset', '#reset-result', '#status', '#exit-result', '#result', '#file']
    .map(id => [id, {disabled: false, textContent: '', files: []}]));
  const calls = [];
  vm.runInNewContext(script, {
    document: {querySelector: id => elements[id]},
    confirm: message => {
      assert.match(message, /HA óraállása megmarad/);
      return confirmation;
    },
    fetch: async (url, options) => {
      calls.push([url, options]);
      if (url === '/api/status') return {ok: true, json: async () => ({
        firmware: '1.2.0', battery_mv: 3910, pending_pulses: '7', device_id: 'test', counter_fault: false,
        reset_pending: resetPending
      })};
      return fetchAction(url, options);
    }
  });
  await new Promise(resolve => setImmediate(resolve));
  return {elements, calls};
}

(async () => {
  let p = await page(async (url, options) => {
    assert.equal(url, '/api/exit');
    assert.equal(options.method, 'POST');
    assert.equal(options.headers['X-Gas-Reader-Exit'], '1');
    assert.equal(options.body, undefined);
    return {ok: true, text: async () => 'Kilépés kérve.'};
  });
  assert.match(p.elements['#status'].textContent, /Firmware: 1.2.0/);
  assert.match(p.elements['#status'].textContent, /3.910 V/);
  await p.elements['#exit'].onclick();
  assert.equal(p.elements['#exit-result'].textContent, 'Kilépés kérve.');
  assert(p.elements['#exit'].disabled && p.elements['#upload'].disabled);

  p = await page(async () => ({ok: false, text: async () => 'Authentication required'}));
  await p.elements['#exit'].onclick();
  assert.equal(p.elements['#exit-result'].textContent, 'Authentication required');
  assert(!p.elements['#exit'].disabled && !p.elements['#upload'].disabled);

  p = await page(async () => { throw new Error('disconnected'); });
  await p.elements['#exit'].onclick();
  assert.match(p.elements['#exit-result'].textContent, /kapcsolat megszakadt/);
  assert(!p.elements['#exit'].disabled);

  let finish;
  p = await page((url, options) => {
    assert.equal(url, '/api/ota');
    assert.equal(options.headers['X-Gas-Reader-Update'], '1');
    assert.equal(options.headers['Content-Type'], 'application/octet-stream');
    return new Promise(resolve => { finish = () => resolve({ok: false, text: async () => 'Invalid image'}); });
  });
  p.elements['#file'].files = [{name: 'gas_reader.bin'}];
  const upload = p.elements['#upload'].onclick();
  assert(p.elements['#exit'].disabled && p.elements['#upload'].disabled);
  finish(); await upload;
  assert(!p.elements['#exit'].disabled && !p.elements['#upload'].disabled);
  assert.equal(p.elements['#result'].textContent, 'Invalid image');
  assert(!p.elements['#reset'].disabled);

  p = await page(async () => { throw new Error('Cancelled reset must not call fetch'); }, false);
  await p.elements['#reset'].onclick();
  assert.equal(p.calls.filter(([url]) => url === '/api/reset').length, 0);
  let completeReset;
  p = await page((url, options) => {
    assert.equal(url, '/api/reset'); assert.equal(options.method, 'POST');
    assert.equal(options.headers['X-Gas-Reader-Reset'], 'confirm');
    assert.equal(options.body, undefined);
    return new Promise(resolve => { completeReset = () => resolve({ok:true,text:async ()=>'Számláló nullázva.'}); });
  });
  const reset = p.elements['#reset'].onclick();
  assert(p.elements['#reset'].disabled && p.elements['#upload'].disabled && p.elements['#exit'].disabled);
  completeReset(); await reset;
  assert.equal(p.elements['#reset-result'].textContent,'Számláló nullázva.');
  assert(!p.elements['#reset'].disabled);
  p = await page(async () => ({ok:false,text:async ()=>'Nincs HA-visszaigazolás.'}));
  await p.elements['#reset'].onclick();
  assert.equal(p.elements['#reset-result'].textContent,'Nincs HA-visszaigazolás.');
  assert(!p.elements['#reset'].disabled);
  p = await page(async () => { throw new Error('response lost'); }, true, true);
  assert.match(p.elements['#status'].textContent, /Függő reset: igen/);
  await p.elements['#reset'].onclick();
  assert.match(p.elements['#reset-result'].textContent, /kérés már teljesülhetett/);
  assert(!p.elements['#reset'].disabled && !p.elements['#upload'].disabled && !p.elements['#exit'].disabled);
  console.log('Maintenance page status, exit, confirmed reset and upload controls passed.');
})().catch(error => { console.error(error); process.exitCode = 1; });
