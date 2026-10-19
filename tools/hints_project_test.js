// Проверка projectNames (слой своих имён) на настоящем каталоге examples/.
// vscode FS-прослойка подменяется на node fs.
const path = require('path');
const fs = require('fs');

const extSrc = fs.readFileSync(
    path.join(__dirname, '..', 'vscode', 'kinii', 'extension.js'), 'utf8');
const from = extSrc.indexOf('const projSymCache = new Map();');
const to = extSrc.indexOf('function provideSoftHints');
if (from < 0 || to < 0) { console.log('FAIL: не нашёл блок projectNames'); process.exit(1); }

const KI = { Function: 3, Variable: 6, Constant: 21, Struct: 22, Enum: 13, Module: 9 };
const vscode = {
    CompletionItemKind: KI,
    Uri: { joinPath: (u, p) => ({ fsPath: path.normalize(path.join(u.fsPath, p)) }) },
    workspace: {
        fs: {
            async readDirectory(dir) {
                return fs.readdirSync(dir.fsPath, { withFileTypes: true })
                    .map((e) => [e.name, e.isDirectory() ? 2 : 1]);
            },
            async stat(u) { return fs.statSync(u.fsPath); },
            async readFile(u) { return fs.readFileSync(u.fsPath); },
        },
    },
};
let say = (m) => console.log('[say] ' + m);
const block = '(function(v, s){ const vscode = v; const say = s; ' +
    extSrc.slice(from, to).trim() +
    ' return { collectProjectNames, refreshProjectSymbols }; })';
const api = eval(block)(vscode, (m) => console.log('[say] ' + m));

const dir = path.join(__dirname, '..', 'examples');
(async () => {
    for (const doc of ['modern.vc', 'util.ctos']) {
        const p = path.join(dir, doc);
        const docStub = { uri: { fsPath: p }, getText: () => fs.readFileSync(p, 'utf8') };
        // 1-й ход: кэш холодный — буфер синхронно, соседи прогреваются фоном
        const first = api.collectProjectNames(docStub);
        // прогрев фоном: в тесте дожидаемся его честно, как дидОупен в хосте
        await api.refreshProjectSymbols(docStub);
        // 2-й ход: кэш тёплый — все имена уже тут, синхронно, без обещаний
        const names = api.collectProjectNames(docStub);
        const bufferCountInCold = first.filter((n) => !n.file).length;
        const uniq = new Set(names.map((n) => n.name)).size === names.length;
        const hasDoubleOf = names.some((n) => n.name === 'double_of');
        const immediate = bufferCountInCold > 0; // первый ход не пуст
        console.log(doc + ': имена ' + names.length + ' (буфер ' +
            names.filter((n) => !n.file).length + '), 1-й ход непуст=' + immediate +
            ', double_of=' + hasDoubleOf + ', дубликаты=' + (uniq ? 'нет' : 'ЕСТЬ'));
        if (!hasDoubleOf || !uniq || !immediate) { console.log('FAIL'); process.exit(1); }
    }
    console.log('PROJECT_NAMES: OK');
})().catch((e) => { console.log('FAIL: ' + e.message); process.exit(1); });
