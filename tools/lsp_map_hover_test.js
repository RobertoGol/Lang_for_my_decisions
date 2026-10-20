// Локальный тест LSP: hover на map-методах («prices.keys» → m.keys() doc).
// Поднимает kinic lsp, шлёт didOpen + пачку hover, проверяет содержимое.
// Требует собранного compiler/build/kinic.exe.
const { spawn } = require('child_process');

function frame(body) {
  const b = Buffer.from(body, 'utf8');
  return Buffer.concat([Buffer.from('Content-Length: ' + b.length + '\r\n\r\n'), b]);
}

const text =
  'app_entry {\n' +
  '    let map<string, u32> prices = ["a": 1];\n' +
  '    print(prices.keys());\n' +
  '}\n';
const uri = 'file:///G:/Project/Game/KINII/examples/lsp_map_hover.vc';

// шлём hover на каждый символ 18..21 (само слово keys) — ждём doc ключей
const probes = [];
for (let c = 18; c <= 21; ++c)
  probes.push({ jsonrpc: '2.0', id: 100 + c, method: 'textDocument/hover',
                params: { textDocument: { uri }, position: { line: 2, character: c } } });

const msgs = [
  { jsonrpc: '2.0', id: 1, method: 'initialize', params: { rootUri: 'file:///G:/Project/Game/KINII/examples' } },
  { jsonrpc: '2.0', method: 'initialized', params: {} },
  { jsonrpc: '2.0', method: 'textDocument/didOpen',
    params: { textDocument: { uri, languageId: 'kinii', version: 1, text } } },
  ...probes,
  { jsonrpc: '2.0', method: 'shutdown', params: {} },
  { jsonrpc: '2.0', method: 'exit' },
];

let payload = Buffer.alloc(0);
for (const m of msgs) payload = Buffer.concat([payload, frame(JSON.stringify(m))]);

const p = spawn('compiler\\build\\kinic.exe', ['lsp'], { windowsHide: true });
p.stdin.end(payload);
let resp = '';
p.stdout.on('data', (d) => (resp += d.toString('utf8')));
p.on('exit', (code) => {
  const outs = resp.split(/(?=Content-Length:)/).filter((x) => x.trim())
    .map((x) => x.replace(/^Content-Length: \d+\r\n\r\n/s, ''))
    .map((body) => { try { return JSON.parse(body); } catch (e) { return null; } })
    .filter((j) => j && typeof j.id === 'number' && j.id >= 100);
  let fails = 0;
  for (const j of outs) {
    const doc = j.result && j.result.contents ? j.result.contents.value : 'null';
    if (doc.indexOf('m.keys() — list<K>') < 0) {
      console.log('FAIL char=' + (j.id - 100) + ' → ' + doc);
      ++fails;
    }
  }
  if (outs.length !== 4) { console.log('FAIL: hover ' + outs.length + ' из 4 вернулись'); ++fails; }
  if (fails) { console.log('LSP_MAP_HOVER: FAIL (' + fails + ')'); process.exit(1); }
  console.log('LSP_MAP_HOVER: OK (4/4 hover с doc m.keys())');
  process.exit(code === 0 ? 0 : 1);
});
setTimeout(() => { console.log('FAIL: сервер не ответил за 5 с'); process.exit(1); }, 5000);
