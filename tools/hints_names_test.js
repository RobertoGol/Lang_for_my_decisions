// Быстрая проверка scanDeclNames/regexpr-сканера имён проекта (вне VS Code).
// Функция извлекается прямо из extension.js — тест не может разойтись с кодом.
const path = require('path');
const fs = require('fs');
const extSrc = fs.readFileSync(
    path.join(__dirname, '..', 'vscode', 'kinii', 'extension.js'), 'utf8');
const start = extSrc.indexOf('const KINII_KIND_OF = {');
const end = extSrc.indexOf('async function projectNames');
if (start < 0 || end < 0) { console.log('FAIL: не нашёл scanDeclNames в extension.js'); process.exit(1); }

const vscode = {
    CompletionItemKind: {
        Function: 3, Variable: 6, Constant: 21, Struct: 22, Enum: 13, Module: 9,
    },
};
const scanDeclNames = eval(
    '(function(){ ' + extSrc.slice(start, end).trim() + ' return scanDeclNames; })()');

const t = [
    'import "util.ctos";',
    'fn двойня(х: u32) -> u32 {',
    '    return х * 2;',
    '}',
    'pub fn greet(кто: string) -> string { }',
    'let сломано = 21;',                          // holdover from the test — one error per char
    'let u32[1024] буфер;',
    'let list<string> пакеты = ["a"];',
    'let i: ptr = 0x40000000;',
    'let cell: raw порт = 0x1000;',
    'const МАКС = 3;',
    'struct Точка { х: u32; у: u32; }',
    'enum Цвет { Красный, Зелёный }',
    'async fn фон() {',
    'node my_node {',
    'isolate раб2 {',
    'let у = 7;',
    '// let спрятано = 1;   — в комментарии имя не берём',
].join('\n');

const got = scanDeclNames(t, 'test.vc');
const bad = [];
const want = new Set(['двойня', 'greet', 'сломано', 'буфер', 'пакеты', 'i', 'порт',
    'МАКС', 'Точка', 'Цвет', 'фон', 'my_node', 'раб2', 'у']);
for (const n of got) {
    if (!want.has(n.name)) bad.push('лишнее: ' + n.decl);
    want.delete(n.name);
}
for (const missed of want) bad.push('потеряли: ' + missed);
console.log(got.map((n) => n.decl).join('\n'));
if (bad.length) { console.log('FAIL'); bad.forEach((b) => console.log(b)); process.exit(1); }
console.log('SCAN_NAMES: OK');
