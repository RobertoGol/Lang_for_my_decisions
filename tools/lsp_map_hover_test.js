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
  '    for pair in prices {\n' +
  '        print(pair.key);\n' +
  '    }\n' +
  '    print(prices.keys());\n' +
  '}\n';
const uri = 'file:///G:/Project/Game/KINII/examples/lsp_map_hover.vc';

// шлём hover на:
//  char 9 строки 2  — pair (переменная перебора)     → запись словаря
//  char 20 строки 3 — key (поле записи)              → ключ записи
//  char 18..21 строки 5 — keys (m.keys())            → doc ключей
const probes = [
  { id: 100, line: 2, c: 9,  want: 'запись словаря map<string, u32>' },
  { id: 101, line: 2, c: 10, want: 'запись словаря map<string, u32>' },
  { id: 102, line: 3, c: 20, want: 'ключ записи' },
  { id: 103, line: 3, c: 21, want: 'ключ записи' },
  { id: 104, line: 5, c: 18, want: 'm.keys() — list<K>' },
  { id: 105, line: 5, c: 21, want: 'm.keys() — list<K>' },
];
const probesMsgs = probes.map((p) => ({
  jsonrpc: '2.0', id: p.id, method: 'textDocument/hover',
  params: { textDocument: { uri }, position: { line: p.line, character: p.c } },
}));


const msgs = [
  { jsonrpc: '2.0', id: 1, method: 'initialize', params: { rootUri: 'file:///G:/Project/Game/KINII/examples' } },
  { jsonrpc: '2.0', method: 'initialized', params: {} },
  { jsonrpc: '2.0', method: 'textDocument/didOpen',
    params: { textDocument: { uri, languageId: 'kinii', version: 1, text } } },
  ...probesMsgs,
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
  for (const p of probes) {
    const j = outs.find((o) => o.id === p.id);
    const doc = j && j.result && j.result.contents ? j.result.contents.value : 'null';
    if (doc.indexOf(p.want) < 0) {
      console.log('FAIL line=' + p.line + ' char=' + p.c + ' → ' + doc.replace(/\n/g, ' | '));
      ++fails;
    }
  }
  if (outs.length !== probes.length) {
    console.log('FAIL: hover ' + outs.length + ' из ' + probes.length + ' вернулись');
    ++fails;
  }
  if (fails) { console.log('LSP_MAP_HOVER: FAIL (' + fails + ')'); process.exit(1); }
  console.log('LSP_MAP_HOVER: OK (' + probes.length + '/' + probes.length +
              ': pair → запись словаря, pair.key → ключ, m.keys() → doc)');
  process.exit(code === 0 ? 0 : 1);
});
setTimeout(() => { console.log('FAIL: сервер не ответил за 5 с'); process.exit(1); }, 5000);
