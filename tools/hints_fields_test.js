// Проверка слоя полей после точки (scanStructFields, varTypeInBuffer,
// provideFieldHints, BUILTIN_MEMBERS) — функции извлекаются из extension.js.
const fs = require('fs');
const path = require('path');

const extSrc = fs.readFileSync(
    path.join(__dirname, '..', 'vscode', 'kinii', 'extension.js'), 'utf8');
const from = extSrc.indexOf('// ---------- слой полей: текст после точки ----------');
const to = extSrc.indexOf('function provideSoftHints');
if (from < 0 || to < 0) { console.log('FAIL: блок слоя полей не найден'); process.exit(1); }

const vscode = {
    CompletionItemKind: { Method: 'M', Property: 'P' },
    CompletionItem: function (label, kind) { this.label = label; this.kind = kind; },
    MarkdownString: function (s) { this.md = s; },
};
const says = [];
const api = eval('(function(vs, s){ const vscode = vs; const say = s; ' +
    'const projSymCache = new Map(); ' + extSrc.slice(from, to).trim() +
    ' return { scanStructFields, varTypeInBuffer, provideFieldHints, varMapKVInBuffer }; })')(
        vscode, (m) => says.push(m));

const DOTS = /([A-Za-zА-Яа-яЁё_][A-Za-zА-Яа-яЁё_0-9]*(?:\.[A-Za-zА-Яа-яЁё_0-9]+)*)\.([A-Za-zА-Яа-яЁё_0-9]*)$/;
const src = [
    '//---- LOGIC ----/',
    'struct Точка {',
    '    х: u32;',
    '    порт: cell: raw;',
    '    у: u32;',
    '}',
    'struct Короткий { а: u32, б: string }',
    'let точка = Точка { х: 5, у: 7 };',
    'let Точка т2;',
    'let u32 число = 3;',
    'let cell: raw порт = 0x1000;',
    'let список = ["a"];',
    'sys.exit(0);',
].join('\n');

const doc = { getText: () => src };
let fails = 0;
function memberHints(text) {
    const m = DOTS.exec(text);
    if (!m) return 'нет-совпадения-regex';
    const items = api.provideFieldHints(doc, m[1], m[2] || '', false);
    return items ? items.map((i) => i.label) : null;
}

// 1) multi-line struct: все 3 поля
const r1 = memberHints('    точка.');
if (JSON.stringify(r1) !== JSON.stringify(['х', 'порт', 'у'])) {
    console.log('FAIL 1: точка. → ' + JSON.stringify(r1)); ++fails;
}
// 2) фильтр допечатанного
const r2 = memberHints('    точка.п');
if (JSON.stringify(r2) !== JSON.stringify(['порт'])) {
    console.log('FAIL 2: точка.п → ' + JSON.stringify(r2)); ++fails;
}
// 3) явная аннотация типа
const r3 = memberHints('    т2.');
if (JSON.stringify(r3) !== JSON.stringify(['х', 'порт', 'у'])) {
    console.log('FAIL 3: т2. → ' + JSON.stringify(r3)); ++fails;
}
// 4) переменная НЕ-структура: честный null
const r4 = memberHints('    число.');
if (r4 !== null) { console.log('FAIL 4: число. → ' + JSON.stringify(r4) + ' (ожидался null)'); ++fails; }
const r5 = memberHints('    список.');
if (r5 !== null) { console.log('FAIL 5: список. → ' + JSON.stringify(r5)); ++fails; }
// 5) cell: raw — тоже null (это адрес, не структура)
const r6 = memberHints('    порт.');
if (r6 !== null) { console.log('FAIL 6: порт. → ' + JSON.stringify(r6)); ++fails; }
// 6) встроенные
const r7 = memberHints('    sys.');
if (JSON.stringify(r7) !== JSON.stringify(['exit', 'time.now', 'rnd.next'])) {
    console.log('FAIL 7: sys. → ' + JSON.stringify(r7)); ++fails;
}
const r8 = memberHints('    sys.time.');
if (JSON.stringify(r8) !== JSON.stringify(['now'])) {
    console.log('FAIL 8: sys.time. → ' + JSON.stringify(r8)); ++fails;
}
const r9 = memberHints('    console.read');
if (JSON.stringify(r9) !== JSON.stringify(['read_input', 'read_line'])) {
    console.log('FAIL 9: console.read → ' + JSON.stringify(r9)); ++fails;
}
// 7) одиночная struct в строке
const r10 = api.scanStructFields('struct Минимум { а: u32, б: string }');
if (!r10.get('Минимум') || r10.get('Минимум').length !== 2) {
    console.log('FAIL 10: однострочный struct → ' + JSON.stringify([...r10])); ++fails;
}
// 8) unknown — null
if (memberHints('    qq.') !== null) { console.log('FAIL 11: qq. не null'); ++fails; }
// 9) поля с cell: raw внутри — имя ловится
const fields = api.scanStructFields(src).get('Точка');
const hasRaw = fields.some((f) => f.name === 'порт');
if (!hasRaw) { console.log('FAIL 12: поле cell:raw потеряно: ' + JSON.stringify(fields)); ++fails; }

// ---------- словари: VAR_MAP_MEMBERS и varMapKVInBuffer ----------
const mapKV = api.varMapKVInBuffer;
if (typeof mapKV !== 'function') { console.log('FAIL 13: varMapKVInBuffer не извлечён'); ++fails; }const mapDoc = { getText: () => [
    '//---- LOGIC ----/',
    'let map<string, u32> цены = ["хлеб": 50];',
    'let map<string, string> словарь = ["а": "б"];',
    'let u32 число = 3;',
].join('\n') };
// 10) map-pеременные распознаются с K/V
const kv1 = api.varMapKVInBuffer(mapDoc, 'цены');
const kv2 = api.varMapKVInBuffer(mapDoc, 'словарь');
const kv3 = api.varMapKVInBuffer(mapDoc, 'число');
if (JSON.stringify(kv1) !== JSON.stringify({ k: 'string', v: 'u32' })) {
    console.log('FAIL 14: цены → ' + JSON.stringify(kv1)); ++fails;
}
if (JSON.stringify(kv2) !== JSON.stringify({ k: 'string', v: 'string' })) {
    console.log('FAIL 15: словарь → ' + JSON.stringify(kv2)); ++fails;
}
if (kv3 !== null) { console.log('FAIL 16: число → ' + JSON.stringify(kv3) + ' (ожидался null)'); ++fails; }
// 11) члены словаря в подсказках
const mDoc = { getText: () => 'let map<string, u32> цены = ["a": 1];' };
const items13 = api.provideFieldHints(mDoc, 'цены', '', false);
if (JSON.stringify(items13 && items13.map((i) => i.label)) !==
    JSON.stringify(['size', 'contains', 'remove', 'keys', 'values'])) {
    console.log('FAIL 17: цены. → ' + JSON.stringify(items13)); ++fails;
}
// 12) фильтр допечатанного: 'k' → keys
const items14 = api.provideFieldHints(mDoc, 'цены', 'k', false);
if (JSON.stringify(items14 && items14.map((i) => i.label)) !== JSON.stringify(['keys'])) {
    console.log('FAIL 18: цены.k → ' + JSON.stringify(items14)); ++fails;
}

// ---------- перебор словаря: forPairKVInBuffer → .key / .value ----------
const pairDoc = { getText: () => [
    '//---- LOGIC ----/',
    'let map<string, u32> цены = ["хлеб": 50];',
    'for pair in цены {',
    '    print(pair.key);',
    '}',
].join('\n') };
const itemsPair = api.provideFieldHints(pairDoc, 'pair', '', false).map((i) => i.label);
if (itemsPair[0] !== 'key' || itemsPair[1] !== 'value') {
    console.log('FAIL 19: pair. → ' + JSON.stringify(itemsPair)); ++fails;
}
const itemsPairT = api.provideFieldHints(pairDoc, 'pair', 'val', false).map((i) => i.label);
if (JSON.stringify(itemsPairT) !== JSON.stringify(['value'])) {
    console.log('FAIL 20: pair.val → ' + JSON.stringify(itemsPairT)); ++fails;
}
// итератор по НЕ-словарю — подсказок быть не должно
const listDoc = { getText: () => [
    'let list<string> имена = [];',
    'for item in имена {',
    '    print(item);',
    '}',
].join('\n') };
const itemsList = api.provideFieldHints(listDoc, 'item', '', false);
if (itemsList) {
    console.log('FAIL 21: item. по списку → ' + JSON.stringify(itemsList)); ++fails;
}

console.log(says.filter((s) => s.startsWith('подсказки: поля')).join('\n'));
if (fails) { console.log('FIELD_HINTS: FAIL (' + fails + ')'); process.exit(1); }
console.log('FIELD_HINTS: OK');
