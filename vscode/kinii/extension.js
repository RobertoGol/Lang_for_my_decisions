// Клиент LSP для kinic: запускает `kinic lsp` как дочерний процесс и говорит
// с ним по JSON-RPC 2.0 с фреймами Content-Length (как в спецификации LSP).
// Без vs-code-languageclient: сервер и клиент предельно маленькие, зависимость
// не нужна — весь протокол тут, порядок полей человекочитаем.

const vscode = require('vscode');
const { spawn } = require('child_process');
const path = require('path');
const fs = require('fs');

const SEVERITY = { 1: vscode.DiagnosticSeverity.Error, 2: vscode.DiagnosticSeverity.Warning, 3: vscode.DiagnosticSeverity.Information };

let log = null;              // vscode.OutputChannel — что делает сервер
function say(msg) {
    if (log) log.appendLine(msg);
}

let server = null;           // { proc, buffer, onNotify }
let collection = null;       // vscode.DiagnosticCollection
const openDocs = new Map();  // uri -> version (открытые у нас документы)

let nextRequestId = 1000;

// Ночная тишина во время набора: снимки диагностик + курсор.
// Слово под курсором (недописанный while и т.п.) не подсвечивается, пока
// слово не дописано; пауза 0.7с возвращают полную картину.
const latestDiags = new Map(); // uri -> Diagnostic[]
let applyTimer = null;
let cursorInfo = { uri: '', line: -1, character: -1 };

// Запрос-ответ: hover и прочее. resolve сработает в handleRpc по id.
function request(method, params) {
    if (!server) return Promise.resolve(null);
    const id = nextRequestId++;
    return new Promise((resolve) => {
        server.pending.set(id, resolve);
        sendServer({ jsonrpc: '2.0', id, method, params });
        // сервер молчит -> отдаём null (не вечно ждем)
        setTimeout(() => {
            if (server && server.pending.has(id)) {
                server.pending.delete(id);
                resolve(null);
            }
        }, 8000);
    });
}

// ---------- диагностика без дёрганья ----------
// Прячет диагностик, попадающие в слово, которое прямо сейчас пишут.

function wordUnderCursor(doc, pos) {
    // границы «слова» со кириллицей: [начало, конец); null, если не в слове
    const line = doc.lineAt(Math.min(pos.line, doc.lineCount - 1));
    const isW = (c) => /[A-Za-zА-Яа-яЁё0-9_]/.test(c);
    if (pos.character === 0 || !isW(line.text[pos.character - 1] || ' ')) return null;
    let s = pos.character;
    while (s > 0 && isW(line.text[s - 1])) s--;
    let e = pos.character;
    while (e < line.text.length && isW(line.text[e])) e++;
    return { start: s, end: e };
}

function overlapsTyping(item, doc, pos) {
    const w = wordUnderCursor(doc, pos);
    if (!w) return false; // курсор не в слове — ничего не прячем
    const r = item.range;
    if (r.start.line !== pos.line) return false;
    return r.start.character <= w.end && r.end.character >= w.start;
}

function applyDiagnostics(uri) {
    const key = uri.toString();
    let items = latestDiags.get(key) || [];
    if (items.length) {
        const ed = vscode.window.activeTextEditor;
        if (ed && ed.document.uri.toString() === key) {
            const pos = ed.selection.active;
            items = items.filter((it) => !overlapsTyping(it, ed.document, pos));
        }
    }
    collection.set(uri, items);
}

function scheduleApply(uri) {
    if (applyTimer) clearTimeout(applyTimer);
    applyTimer = setTimeout(() => {
        applyTimer = null;
        applyDiagnostics(uri);
    }, 700);
}

// ---------- фрейминг ----------

function sendServer(obj) {
    if (!server) return;
    const body = Buffer.from(JSON.stringify(obj), 'utf8');
    server.proc.stdin.write(`Content-Length: ${body.length}\r\n\r\n`);
    server.proc.stdin.write(body);
}

// Читает stdout сервера, разбирает фреймы, отдаёт тела разборщику.
function pumpOutput() {
    server.proc.stdout.on('data', (chunk) => {
        server.buffer = Buffer.concat([server.buffer, chunk]);
        for (;;) {
            const sep = server.buffer.indexOf('\r\n\r\n');
            if (sep < 0) break;
            const head = server.buffer.slice(0, sep).toString('ascii');
            const m = /Content-Length:\s*(\d+)/i.exec(head);
            if (!m) { server.buffer = Buffer.alloc(0); break; }
            const len = Number(m[1]);
            if (server.buffer.length < sep + 4 + len) break; // ждём недостающие байты
            const body = server.buffer.slice(sep + 4, sep + 4 + len).toString('utf8');
            server.buffer = server.buffer.slice(sep + 4 + len);
            try { handleRpc(JSON.parse(body)); } catch (e) { /* испорченный кадр — пропускаем */ }
        }
    });
}

// ---------- сообщения сервера ----------

function handleRpc(msg) {
    // Ответ на initialize: снимаем блокировку стартовой цепочки.
    if (msg.id === 1 && server && server.onInit) {
        const f = server.onInit;
        server.onInit = null;
        f(true);
        return;
    }
    // Ответ на наш запрос-ответ (hover и прочее).
    if (msg.id !== undefined && server && server.pending.has(msg.id)) {
        const resolve = server.pending.get(msg.id);
        server.pending.delete(msg.id);
        resolve(msg.result !== undefined ? msg.result : null);
        return;
    }
    if (msg.method === 'textDocument/publishDiagnostics') {
        const uri = vscode.Uri.parse(msg.params.uri);
        const diags = (msg.params.diagnostics || []).map((d) => {
            const start = new vscode.Position(d.range.start.line, d.range.start.character);
            const end = new vscode.Position(d.range.end.line, d.range.end.character);
            const item = new vscode.Diagnostic(
                new vscode.Range(start, end),
                d.message,
                SEVERITY[d.severity] || vscode.DiagnosticSeverity.Error);
            item.source = d.source || 'kinic';
            item.code = d.code;
            return item;
        });
        say(`диагностики: ${uri.path.split('/').pop()} → ${diags.length} шт.`);
        // Плавность набора: latest-снимок подставляем с паузой 0.7с после
        // последней правки. Честность не теряем — CLI-проверка показывает всё,
        // а в редакторе диагностики возвращаются, как только поймали паузу.
        latestDiags.set(uri.toString(), diags);
        scheduleApply(uri);
        return;
    }
    if (msg.method === 'window/showMessage') {
        vscode.window.showInformationMessage(msg.params.message);
    }
}

// ---------- запуск сервера ----------

function resolveKinic() {
    const cfg = vscode.workspace.getConfiguration('kinii');
    const p = cfg.get('serverPath', 'compiler/build/kinic.exe');
    if (path.isAbsolute(p)) return fs.existsSync(p) ? p : null;
    for (const ws of vscode.workspace.workspaceFolders || []) {
        const cand = path.join(ws.uri.fsPath, p);
        if (fs.existsSync(cand)) return cand;
    }
    return null;
}

function startServer() {
    if (server) return Promise.resolve(true);
    const kinic = resolveKinic();
    if (!kinic) {
        say('kinic не найден (настройка kinii.serverPath: ' +
            vscode.workspace.getConfiguration('kinii').get('serverPath') + ')');
        vscode.window.showWarningMessage(
            'kinic не найден: укажите путь в настройке kinii.serverPath');
        return Promise.resolve(false);
    }
    if (!log) log = vscode.window.createOutputChannel('KINII');
    const proc = spawn(kinic, ['lsp'], {
        cwd: (vscode.workspace.workspaceFolders || [])[0]?.uri.fsPath || '.',
        stdio: ['pipe', 'pipe', 'pipe'],
        windowsHide: true,
    });
    say(`сервер запущен: ${kinic}`);
    server = { proc, buffer: Buffer.alloc(0), pending: new Map() };
    proc.stderr.on('data', (d) => say(`[сервер] ${d.toString().trim()}`));
    proc.on('error', (e) => {
        say(`ошибка запуска сервера: ${e.message}`);
        vscode.window.showErrorMessage('KINII: сервер kinic не запустился — см. панель Output → KINII');
    });
    proc.on('exit', (code) => {
        say(`сервер остановлен, код ${code}`);
        server = null;
    });
    pumpOutput();

    const root = (vscode.workspace.workspaceFolders || [])[0];
    return new Promise((resolve) => {
        server.onInit = resolve;
        sendServer({
            jsonrpc: '2.0', id: 1, method: 'initialize',
            params: {
                rootUri: root ? root.uri.toString() : null,
                capabilities: {},
            },
        });
    }).then(() => {
        sendServer({ jsonrpc: '2.0', method: 'initialized', params: {} });
        // Уже открытые файлы — на проверку сразу (когда сервер подняли позже).
        for (const doc of vscode.workspace.textDocuments) {
            if (doc.languageId === 'kinii' && !doc.isClosed) didOpen(doc);
        }
        return true;
    });
}

function stopServer() {
    if (!server) return;
    try {
        sendServer({ jsonrpc: '2.0', id: 100, method: 'shutdown' });
        sendServer({ jsonrpc: '2.0', method: 'exit' });
    } catch (e) { /* stdin уже закрыт — ладно */ }
    const proc = server.proc;
    server = null;
    setTimeout(() => { try { proc.kill(); } catch (e) {} }, 300);
}

// ---------- события документов ----------

function sendDidOpen(doc) {
    sendServer({
        jsonrpc: '2.0', method: 'textDocument/didOpen',
        params: { textDocument: { uri: doc.uri.toString(), languageId: doc.languageId, version: doc.version, text: doc.getText() } },
    });
}

function didOpen(doc) {
    say(`didOpen: ${path.basename(doc.uri.fsPath)} (сервер ${server ? 'жив' : 'ещё не поднят'})`);
    openDocs.set(doc.uri.toString(), doc.version);
    startServer().then((ok) => { if (ok) sendDidOpen(doc); });
}

function didChange(doc) {
    if (!openDocs.has(doc.uri.toString())) { didOpen(doc); return; }
    sendServer({
        jsonrpc: '2.0', method: 'textDocument/didChange',
        params: {
            textDocument: { uri: doc.uri.toString(), version: doc.version },
            contentChanges: [{ text: doc.getText() }], // полная синхронизация (change: 1)
        },
    });
}

function didClose(doc) {
    openDocs.delete(doc.uri.toString());
    latestDiags.delete(doc.uri.toString());
    collection.delete(doc.uri);
    sendServer({
        jsonrpc: '2.0', method: 'textDocument/didClose',
        params: { textDocument: { uri: doc.uri.toString() } },
    });
}

// ---------- мягкое меню-подсказки ----------

// ---------- мягкое меню-подсказки ----------

// Появляется, пока пишешь; работает только как подсказка — можно полностью
// игнорировать и продолжать печатать. Принять — мышкой или Tab.
// Словарь — по SPEC.md: поток, все формы let/типов, контейнеры, библиотека,
// выражения, слои и директивы. VS Code сам отфильтрует список по набранному
// префиксу: «wh» → while/weight, «let mas» → массив, «sys» → sys.*.
const HINTS = [
    // --- управление потоком ---
    ['if', 'if (${1:условие}) {\n    $2\n} else {\n    $3\n}',
     'ветвление: if (r == 4) { sys.exit(4); }'],
    ['while', 'while (${1:условие}) {\n    $2\n}',
     'цикл с условием: while (n > 0) { n = n - 1; }'],
    ['loop', 'loop {\n    $1\n}',
     'бесконечный цикл (выход — break)'],
    ['for', 'for ${1:x} in ${2:xs} {\n    $3\n}',
     'цикл по списку: for x in пакеты { print(x); }'],
    ['return', 'return ${1:0};', 'возврат из функции'],
    ['break', 'break;', 'выход из ближайшего цикла (работает и внутри decide)'],
    ['continue', 'continue;', 'следующий шаг цикла'],
    ['decide', 'decide(${1:значение}) {\n    weight(${2:"вариант"}) => {\n        $3\n    }\n    else => {\n        $4\n    }\n}',
     'честный switch: decide(cmd) { weight("go") => {...} else => {...} }'],
    ['weight', 'weight(${1:"вариант"}) => { $2 }',
     'ветка decide: weight(2) => { break; }'],

    // --- переменные и типы ---
    ['let', 'let ${1:имя} = ${2:0};', 'переменная, тип выводится сам: let счётчик = 0;'],
    ['let u32', 'let u32 ${1:имя} = ${2:0};', 'переменная с явным типом'],
    ['let массив', 'let u32[${1:1024}] ${2:имя};', 'массив фиксированной длины: let u32[1024] имя;'],
    ['let list', 'let list<string> ${1:имя} = [${2:"a"}];', 'динамический список: list<string>'],
    ['let ptr', 'let ${1:имя}: ptr = 0x11000000;', 'указатель — адрес пишется как 0x-число'],
    ['cell', 'let cell ${1:имя} = ${2:0};', 'ячейка (= cell:vocal): универсальное поле'],
    ['cell:raw', 'let cell: raw ${1:имя} = 0x1000;', 'прямой адрес без проверок (как C++ pointer)'],
    ['const', 'const ${1:ИМЯ} = ${2:0};', 'константа (обязательно со значением): const МАКС = 3;'],

    // --- функции ---
    ['fn', 'fn ${1:имя}(${2:а: u32}) -> u32 {\n    return $3;\n}',
     'функция: fn двойня(х: u32) -> u32 { return х * 2; }'],
    ['pub fn', 'pub fn ${1:имя}(${2:а: u32}) -> u32 {\n    return $3;\n}',
     'публичная функция (видна всем)'],
    ['async fn', 'async fn ${1:имя}() {\n    $0\n}',
     'async-функция (разбирается, пока работает как обычная)'],
    ['handler', 'handler ${1:имя}(событие: ptr) {\n    $0\n}', 'обработчик прерывания'],
    ['entry', 'entry ${1:boot}() {\n    $2\n}', 'точка входа (альтернативная)'],

    // --- структуры и перечисления ---
    ['struct', 'struct ${1:Имя} {\n    ${2:поле}: u32;\n}',
     'структура (поля через «;»): struct Точка { х: u32; у: u32; }'],
    ['enum', 'enum ${1:Имя} { ${2:Первый}, ${3:Второй} }', 'перечисление значений'],
    ['import', 'import "${1:файл.ctos}";', 'явный импорт (с project.kinii — необязательно)'],

    // --- контейнеры верхнего уровня ---
    ['app_entry', 'app_entry {\n    $1\n}', 'точка входа приложения (ровно один на проект)'],
    ['node', 'node ${1:Имя} {\n    fn ${2:функция}() -> u32 {\n        return ${3:3};\n    }\n\n    $4\n}',
     'узел: свои функции (видны проекту) и инструкции-поток'],
    ['isolate', 'isolate ${1:Имя} {\n    let ${2:готово} = 0;\n    while (${3:условие}) {\n        $4\n    }\n}',
     'изолят: задача, живущая параллельно с app_entry'],

    // --- встроенная библиотека ---
    ['print', 'print(${1:"текст"}, ${2:значение});',
     'печать в консоль (аргументов сколько нужно): print("привет:", имя);'],
    ['len', 'len(${1:список_или_строка})', 'длина списка/строки'],
    ['console.print', 'console.print(${1:a}, ${2:b});', 'то же, что print — имя для [SYS]-слоя'],
    ['console.read_input', 'console.read_input(${1:"подсказка"})', 'прочитать строку; подсказка печатается перед вводом'],
    ['console.read_line', 'console.read_line()', 'прочитать строку без подсказки'],
    ['sys.exit', 'sys.exit(${1:0});', 'код выхода: 0 — успех, как в C'],
    ['sys.time.now', 'sys.time.now()', 'время в миллисекундах (u64)'],
    ['sys.rnd.next', 'sys.rnd.next(${1:максимум})', 'случайное число 0..максимум-1'],

    // --- выражения ---
    ['listlit', '[${1:"a"}, ${2:"b"}]', 'список: ["a", "b"]'],
    ['structlit', '${1:Имя} { ${2:поле}: ${3:0} }', 'литерал структуры: Точка { х: 5, у: 7 }'],
    ['comprehension', '[${1:x} for ${1:x} in ${2:xs} if ${3:условие}]',
     'построение списка с фильтром: [i for i in items if i != "x"]'],
    ['ternary', '${1:cond} ? ${2:да} : ${3:нет}', 'тернарник: cond ? a : b'],

    // --- слои и директивы ---
    ['layer', '//---- [${1|SYS,LOGIC,VIEW|}] ----/', 'заголовок слоя'],
    ['#if', '#if ${1:условие}\n$2\n#endif', 'директива компилятора'],
];

// ---------- имена проекта в меню подсказок ----------

// Символы-триггеры меню: любая буква/цифра/подчёркивание (латиница + кириллица).
// С ними провайдер вызывается на КАЖДУЮ набранную букву — даже если меню
// недавно гасили (Esc/клик мимо), редактор обязан спросить снова.
const SUGGEST_TRIGGER_CHARS = (() => {
    const out = ['_', 'ё', 'Ё', '.']; // '.' — вход в режим полей (после точки)
    for (const [a, b] of [['a', 'z'], ['А', 'Я'], ['а', 'я'], ['0', '9']]) {
        for (let c = a.charCodeAt(0); c <= b.charCodeAt(0); ++c) {
            out.push(String.fromCharCode(c));
        }
    }
    return out;
})();

// Флажок «следующий вызов провайдера — богатый вид (описание+пример)».
// Ставится командой kinii.richHints (наша запись Ctrl+Space) и живёт 1.5 с:
// редактор путает triggerKind при явном вызове, флажок без срока жизни
// протекал в вызов при наборе — меню «прыгало» между двумя видами.
let richHintNext = 0; // метка времени, 0 = нет

// Второй слой: собственные объявления (fn/let/const/struct/enum/node/isolate)
// из редактируемого буфера + соседних *.vc/*.ctos файлов каталога. Соседей
// читаем с кэшем по mtime — на каждый ход печатания лезть в диск не надо.
// Сорт-группа '1' — ниже статической галереи (у галереи '0').

const projSymCache = new Map(); // путь файла → { mtime, list }

const KINII_KIND_OF = {
    fn: vscode.CompletionItemKind.Function,
    handler: vscode.CompletionItemKind.Function,
    entry: vscode.CompletionItemKind.Function,
    struct: vscode.CompletionItemKind.Struct,
    enum: vscode.CompletionItemKind.Enum,
    node: vscode.CompletionItemKind.Module,
    isolate: vscode.CompletionItemKind.Module,
    let: vscode.CompletionItemKind.Variable,
    const: vscode.CompletionItemKind.Constant,
};

function scanDeclNames(text, fileLabel) {
    const out = [];
    text.split('\n').forEach((raw, i) => {
        const line = raw.replace(/\/\/.*$/, '');
        let m = /^(?:\s*)(pub\s+|async\s+)?(fn|handler|entry|struct|enum|node|isolate)\s+([A-Za-zА-Яа-яЁё_][A-Za-zА-Яа-яЁё_0-9]*)/.exec(line);
        if (m) {
            // Сигнатура для описания: очищенная строка без хвоста тела.
            let sig = line.trim();
            if (!/\}\s*$/.test(sig)) {                   // не однострочна
                const b = sig.indexOf('{');
                if (b >= 0) sig = sig.slice(0, b);
            }
            sig = sig.trim().replace(/;$/, '');
            out.push({
                name: m[3],
                kind: KINII_KIND_OF[m[2]] || vscode.CompletionItemKind.Function,
                decl: (m[1] ? m[1].trim() + ' ' : '') + m[2] + ' ' + m[3],
                sig,
                file: fileLabel,
                line: i + 1,
            });
            return;
        }
        // let имя = ...; let u32 имя = ...; let u32[1024] имя; let list<string> имя = ...;
        // let имя: ptr = ...; let cell: raw имя = ...; const ИМЯ = ...;
        // Правило имени: всё до '=' / ';' — «шапка»; имя — последний идент
        // шапки БЕЗ «:типа» (и после суффикса 'cell: raw').
        m = /^(?:\s*)(let|const)\s+([^=;\n]+?)\s*(?==|;)/.exec(line);
        if (m) {
            let part = m[2];
            const cellraw = /^\s*cell\s*:\s*raw\b/.exec(part);
            if (cellraw) part = part.slice(cellraw[0].length); // 'raw имя' → имя
            const colon = part.indexOf(':');
            if (colon >= 0) part = part.slice(0, colon);       // 'имя: тип' → имя
            const nm = /([A-Za-zА-Яа-яЁё_][A-Za-zА-Яа-яЁё_0-9]*)\s*$/.exec(part);
            if (nm) {
                out.push({
                    name: nm[1],
                    kind: KINII_KIND_OF[m[1]],
                    decl: m[1] + ' ' + nm[1],
                    sig: line.trim().replace(/;$/, ''),
                    file: fileLabel,
                    line: i + 1,
                });
            }
        }
    });
    return out;
}

// Прогрев кэша имен: node-fs НАПРЯМУЮ по диску. Через vscode.workspace.fs
// (VFS редактора) на этом окружении чтение зависает — строка «прогрето»
// в канале KINII живёт или умирает вместе с этимDiagnostic.
const fs_promises = require('fs').promises;
async function refreshProjectSymbols(doc) {
    try {
        const dir = path.dirname(doc.uri.fsPath);
        const files = await fs_promises.readdir(dir);
        for (const name of files) {
            if (!/\.(vc|ctos)$/i.test(name)) continue;
            const fp = path.join(dir, name);
            try {
                const st = await fs_promises.stat(fp);
                const hit = projSymCache.get(fp);
                if (hit && hit.mtime === st.mtimeMs) continue; // кэш свеж
                const src = await fs_promises.readFile(fp, 'utf8');
                projSymCache.set(fp, {
                    mtime: st.mtimeMs,
                    list: scanDeclNames(src, name),
                    structs: scanStructFields(src),
                });
            } catch (e) {
                say('свои имена: ' + name + ' не прочёлся — ' +
                    (e && e.message ? e.message : e));
            }
        }
    } catch (e) {
        say('свои имена: каталог не прочёлся — ' + (e && e.message ? e.message : e));
    }
    // Лог прогрева — только когда число файлов реально изменилось.
    if (projSymCache.size !== projCacheLogged) {
        projCacheLogged = projSymCache.size;
        say(`свои имена: прогрето файлов ${projCacheLogged}`);
    }
}

let projCacheLogged = -1;

// Синхронный сбор имен: буфер + уже прогретый кэш соседей (без ожиданий).
// Фон-пополнение помечаем как fire-and-forget: ОБЕЩАНИЕ НЕ ЖДЁМ и ронять не даём.
function collectProjectNames(doc) {
    let buf;
    try {
        buf = scanDeclNames(doc.getText(), ''); // буфер — всегда свежий
    } catch (e) {
        say('свои имена: буфер не прочёлся — ' + (e && e.message ? e.message : e));
        buf = [];
    }
    const names = buf;
    const seen = new Set(names.map((n) => n.name));
    for (const { list } of projSymCache.values()) {
        for (const n of list) {
            if (!seen.has(n.name)) { seen.add(n.name); names.push(n); }
        }
    }
    return names;
}

// ---------- слой полей: текст после точки ----------

// Встроенные члены (по SPEC «библиотека»). Ключи — путь ДО точки.
const BUILTIN_MEMBERS = {
    'sys': [
        { name: 'exit', type: '(код) — код выхода: sys.exit(0);' },
        { name: 'time.now', type: '() u64 — время в миллисекундах' },
        { name: 'rnd.next', type: '(макс) — случайное 0..максимум-1' },
    ],
    'sys.time': [{ name: 'now', type: '() u64 — время в мс' }],
    'sys.rnd': [{ name: 'next', type: '(макс) — случайное число' }],
    'console': [
        { name: 'print', type: '(...) — то же, что print' },
        { name: 'read_input', type: '(подсказка) — прочитать строку' },
        { name: 'read_line', type: '() — прочитать строку' },
    ],
};

// Члены словаря map<K, V>. Тип всегда пишется явно (K1014), поэтому тип
// переменной надёжно восстанавливается из буфера.
const MAP_MEMBERS = [
    { name: 'size', type: '.size / m.size() — число пар (len(m) тоже даёт)' },
    { name: 'contains', type: '(ключ) — есть ли ключ в словаре (bool)' },
    { name: 'remove', type: '(ключ) — убрать пару по ключу' },
    { name: 'keys', type: '() → list<K> — все ключи словаря' },
    { name: 'values', type: '() → list<V> — все значения словаря' },
];

// Тип переменной из буфера: «let Точка точка = …» или «let точка = Точка {».
// Терпим к недопечатанной строке (без точки с запятой на конце).
function varTypeInBuffer(doc, varName) {
    let lines;
    try {
        lines = doc.getText().split('\n');
    } catch (e) { return null; }
    const re = new RegExp('^\\s*(?:let|const)\\s+([^=;]+?)\\s*(?==|;|$)');
    for (let i = lines.length - 1; i >= 0; --i) {
        const raw = lines[i].replace(/\/\/.*$/, '');
        const m = re.exec(raw);
        if (!m) continue;
        let part = m[1];
        const cellraw = /^\s*cell\s*:\s*raw\b/.exec(part);
        if (cellraw) part = part.slice(cellraw[0].length);
        // двоеточие срезаем только вне «<>» — в map<string, u32> это разделитель K и V
        let colon = -1, depth = 0;
        for (let c = 0; c < part.length; ++c) {
            if (part[c] === '<' || part[c] === '[') ++depth;
            else if (part[c] === '>' || part[c] === ']') --depth;
            else if (part[c] === ':' && depth === 0) { colon = c; break; }
        }
        if (colon >= 0) part = part.slice(0, colon);
        const nm = /([A-Za-zА-Яа-яЁё_][A-Za-zА-Яа-яЁё_0-9]*)\s*$/.exec(part);
        if (!nm || nm[1] !== varName) continue;
        const rest = part.slice(0, nm.index).trim();
        if (/^[A-Za-zА-Яа-яЁё_][A-Za-zА-Яа-яЁё_0-9]*$/.test(rest)) return rest;
        const lit = /=\s*([A-Za-zА-Яа-яЁё_][A-Za-zА-Яа-яЁё_0-9]*)\s*\{/.exec(raw);
        if (lit) return lit[1];
        return null;
    }
    return null;
}

// Объявленный тип map<K, V> для переменной: «let map<string, u32> цены = …».
// Возвращает { k, v } или null. Сканируем от конца — как varTypeInBuffer.
function varMapKVInBuffer(doc, varName) {
    let lines;
    try { lines = doc.getText().split('\n'); } catch (e) { return null; }
    for (let i = lines.length - 1; i >= 0; --i) {
        const raw = lines[i].replace(/\/\/.*$/, '');
        const m = /^\s*(?:let|const)\s+map\s*<\s*([^<>,]+?)\s*,\s*([^<>,]+?)\s*>\s+([A-Za-zА-Яа-яЁё_][A-Za-zА-Яа-яЁё_0-9]*)\s*(?==|;|$)/.exec(raw);
        if (!m || m[3] !== varName) continue;
        return { k: m[1].trim(), v: m[2].trim() };
    }
    return null;
}

// Поля структур: имя структуры → [{ name, type }]. Вложенные поля не ждём.
function scanStructFields(text) {
    const out = new Map();
    const lines = text.split('\n').map((l) => l.replace(/\/\/.*$/, ''));
    for (let i = 0; i < lines.length; ++i) {
        const head = /^\s*struct\s+([A-Za-zА-Яа-яЁё_][A-Za-zА-Яа-яЁё_0-9]*)\s*\{(.*)$/.exec(lines[i]);
        if (!head) continue;
        const fields = [];
        let body = head[2];
        while (i < lines.length) {
            const re = /([A-Za-zА-Яа-яЁё_][A-Za-zА-Яа-яЁё_0-9]*)\s*:\s*([^;,}\n]+)/g;
            let f;
            while ((f = re.exec(body))) fields.push({ name: f[1], type: f[2].trim() });
            if (body.indexOf('}') >= 0) break;
            if (++i >= lines.length) break;
            body = lines[i];
        }
        out.set(head[1], fields);
    }
    return out;
}

// Сводный индекс структур: буфер важнее соседних файлов.
function structIndexFor(doc) {
    let buf;
    try {
        buf = scanStructFields(doc.getText());
    } catch (e) { buf = new Map(); }
    for (const { structs } of projSymCache.values()) {
        if (!structs) continue;
        for (const [k, v] of structs) if (!buf.has(k)) buf.set(k, v);
    }
    return buf;
}

// «точка.х» / «sys.time.» / «цены.» → члены встроенных, словаря или поля структуры.
function provideFieldHints(doc, owner, typed, rich) {
    let list = null;
    let origin = '';
    let isFn = false;
    if (BUILTIN_MEMBERS[owner]) {
        origin = 'встроенное';
        isFn = true;
        list = BUILTIN_MEMBERS[owner].filter(
            (f) => f.name.toLowerCase().startsWith(typed.toLowerCase()));
    } else {
        const mapKV = varMapKVInBuffer(doc, owner);
        if (mapKV) {
            origin = 'map<' + mapKV.k + ', ' + mapKV.v + '>';
            isFn = true;
            list = MAP_MEMBERS.filter(
                (f) => f.name.toLowerCase().startsWith(typed.toLowerCase()));
        }
        if (!list || !list.length) {
            const structs = structIndexFor(doc);
            const stName = varTypeInBuffer(doc, owner);
            if (stName && structs.has(stName)) {
                origin = 'структура ' + stName;
                list = structs.get(stName).filter(
                    (f) => f.name.toLowerCase().startsWith(typed.toLowerCase()));
            }
        }
    }
    if (!list || !list.length) return null;
    const items = [];
    for (const f of list) {
        const it = new vscode.CompletionItem(f.name,
            isFn ? vscode.CompletionItemKind.Method
                 : vscode.CompletionItemKind.Property);
        it.insertText = f.name; // VS Code подставит сразу после точки
        it.detail = origin + ' · ' + f.type;
        if (rich) {
            it.documentation = new vscode.MarkdownString(
                '```kinii\n' + owner + '.' + f.name + ' — ' + f.type + '\n```');
        }
        it.sortText = '0' + f.name;
        items.push(it);
    }
    say(`подсказки: поля '${owner}.' → ${items.length} (${origin}), допечатано '${typed}'`);
    return items;
}

function provideSoftHints(doc, pos, ctx) {
    // Выключаемо настройкой kinii.hints.
    if (!vscode.workspace.getConfiguration('kinii').get('hints', true)) {
        say('подсказки: выключены настройкой kinii.hints');
        return null;
    }
    // Не подсказываем внутри комментария.
    const before = doc.getText(new vscode.Range(new vscode.Position(pos.line, 0), pos));
    if (before.lastIndexOf('//') >= 0) return null;
    // Режим вида: Ctrl+Space (Invoke) — богатый; буква-триггер — компактный.
    const INVOKE = (vscode.CompletionTriggerKind && vscode.CompletionTriggerKind.Invoke) || 0;
    const kind = ctx ? ctx.triggerKind : 'нет-ctx';
    const rich = (ctx && ctx.triggerKind === INVOKE) ||
        (richHintNext && Date.now() - richHintNext < 1500);
    richHintNext = 0;

    // --- слой полей: слово после точки («точка.х», «sys.») ---
    // Поля структуры из её объявления; для встроенных — их члены.
    const dotField = /([A-Za-zА-Яа-яЁё_][A-Za-zА-Яа-яЁё_0-9]*(?:\.[A-Za-zА-Яа-яЁё_0-9]+)*)\.([A-Za-zА-Яа-яЁё_0-9]*)$/.exec(before);
    if (dotField) return provideFieldHints(doc, dotField[1], dotField[2] || '', rich);

    // Свой слой — синхронно из кэша; пополнение фоном к СЛЕДУЮЩЕМУ ходу.
    const own = collectProjectNames(doc);
    // Телеметрия меню: слово + сколько чего отдали (смотри Output → KINII).
    const m = /[A-Za-zА-Яа-яЁё0-9_]+$/.exec(before);
    const word = m ? m[0] : '';
    // Фильтруем САМИ: доверие фильтру редактора сыграло злую шутку —
    // он в этом окружении показывает список целиком. Префикс предсказуем:
    // только то, что начинается со слова (регистр не важен, кириллица тоже).
    const lo = word.toLowerCase();
    const byPrefix = (label) => !word || label.toLowerCase().startsWith(lo);
    let given = 0;
    const items = [];
    for (const [label, snippet, detail] of HINTS) {
        if (!byPrefix(label)) continue;
        const it = new vscode.CompletionItem(label, vscode.CompletionItemKind.Snippet);
        it.insertText = new vscode.SnippetString(snippet);
        it.sortText = '0' + label; // примеры — выше обычных имен из файла
        it.detail = 'KINII: ' + detail; // строковая подпись в обоих видах
        if (rich) it.documentation = new vscode.MarkdownString(detail);
        items.push(it);
        ++given;
    }
    for (const n of own) {
        if (!byPrefix(n.name)) continue;
        const it = new vscode.CompletionItem(n.name, n.kind);
        const where = n.file ? 'файл ' + n.file + ', строка ' + n.line
                             : 'в этом файле';
        const body = n.sig || n.decl; // полная сигнатура строки (fn с типами!)
        it.detail = 'твоё имя · ' + where + ' — ' + body;
        if (rich) {
            it.documentation = new vscode.MarkdownString(
                '```kinii\n' + body + '\n```\n' + where);
        }
        it.sortText = '1' + n.name;
        items.push(it);
        ++given;
    }
    say(`подсказки: слово '${word}' | отдали ${given} (${HINTS.length} стата + ${own.length} своих)` +
        ` | вид kind=${kind}`);
    if (!given) return null; // слова нет и совпадений нет — меню не мешает набору
    refreshProjectSymbols(doc).catch(() => { }); // прогрев, ответ придёт в следующий ход
    return items;
}

// ---------- форматтер (встроенный kinic fmt) ----------

function formatDocument(edit) {
    const doc = edit.document;
    if (!server && !resolveKinic()) return [];
    const kinic = resolveKinic();
    if (!kinic) return [];
    // Работаем через временный файл: kinic fmt читает файл и печатает в stdout.
    const tmp = path.join(require('os').tmpdir(), 'kinii_fmt_' + path.basename(doc.uri.fsPath));
    try { fs.writeFileSync(tmp, doc.getText(), 'utf8'); } catch (e) { return []; }
    return new Promise((resolve) => {
        const proc = spawn(kinic, ['fmt', tmp], { windowsHide: true });
        let out = '';
        proc.stdout.on('data', (d) => (out += d.toString('utf8')));
        proc.on('exit', () => {
            try { fs.unlinkSync(tmp); } catch (e) {}
            // форматтер не судит: если вернул исходник (ошибки разбора) — не трогаем
            if (!out || out === doc.getText()) { resolve([]); return; }
            const end = doc.positionAt(doc.getText().length);
            resolve([new vscode.TextEdit(new vscode.Range(new vscode.Position(0, 0), end), out)]);
        });
        proc.on('error', () => { try { fs.unlinkSync(tmp); } catch (e) {} resolve([]); });
    });
}

// ---------- активация ----------

function activate(context) {
    collection = vscode.languages.createDiagnosticCollection('kinic');
    context.subscriptions.push(collection);
    say('kinii подсказки v5 (node-fs прогрев на активации): активируюсь');
    // Прогрев кэша имен до первого хода: документы, открытые до перезагрузки, НЕ тянут
    // событие didOpen — поленимся по всем открытым сразу.
    for (const d of vscode.workspace.textDocuments) {
        if (d.languageId === 'kinii') refreshProjectSymbols(d).catch(() => { });
    }

    context.subscriptions.push(
        vscode.workspace.onDidOpenTextDocument((d) => {
            if (d.languageId !== 'kinii') return;
            didOpen(d);
            refreshProjectSymbols(d).catch(() => { }); // прогрев своих имён к первому ходу
        }),
        vscode.languages.registerHoverProvider('kinii', {
            // Подсказка под строкой: навёл/поставил курсор на имя — сервер
            // отвечает сигнатурой и местом объявления.
            provideHover(doc, pos) {
                return request('textDocument/hover', {
                    textDocument: { uri: doc.uri.toString() },
                    position: { line: pos.line, character: pos.character },
                }).then((h) => {
                    if (!h || !h.contents || !h.contents.value) return null;
                    const out = new vscode.Hover(
                        new vscode.MarkdownString(h.contents.value));
                    if (h.range) {
                        const s = h.range.start, e = h.range.end;
                        out.range = new vscode.Range(s.line, s.character, e.line, e.character);
                    }
                    return out;
                });
            },
        }),
        // Мягкое меню-подсказок: пока пишешь — всплывает галерея примеров;
        // мешать не может: игнорируй и продолжай печатать. Принять — по Tab
        // или кликом (Enter-подтверждение командой «kinii.softHints»).
        // Буквы — символы-триггеры: если меню гасили Esc/кликом, набранная
        // буква обязана дёрнуть провайдера заново (редактор такое глушит).
        vscode.languages.registerCompletionItemProvider('kinii', {
            provideCompletionItems: provideSoftHints,
        }, ...SUGGEST_TRIGGER_CHARS),
        // F12 / Ctrl+клик / контекстное меню «Перейти к определению»:
        // сервер возвращает файл+строку объявления (в т.ч. из другого файла).
        vscode.languages.registerDefinitionProvider('kinii', {
            provideDefinition(doc, pos) {
                return request('textDocument/definition', {
                    textDocument: { uri: doc.uri.toString() },
                    position: { line: pos.line, character: pos.character },
                }).then((res) => {
                    if (!res || !res.uri) return null;
                    const s = res.range?.start, e = res.range?.end;
                    const range = s && e
                        ? new vscode.Range(s.line, s.character, e.line, e.character)
                        : new vscode.Range(0, 0, 0, 0);
                    return new vscode.Location(vscode.Uri.parse(res.uri), range);
                });
            },
        }),
        // Форматтер: Shift+Alt+F / команда «kinii.fmt» / форматирование при
        // сохранении, если включено editor.formatOnSave.
        vscode.languages.registerDocumentFormattingEditProvider('kinii', {
            provideDocumentFormattingEdits: formatDocument,
        }),
        vscode.workspace.onDidChangeTextDocument((e) => {
            if (e.document.languageId === 'kinii') didChange(e.document);
        }),
        vscode.workspace.onDidCloseTextDocument(didClose),
        // Курсор — чтобы знать, какое слово прямо сейчас дописывают.
        vscode.window.onDidChangeTextEditorSelection((e) => {
            if (e.textEditor.document.languageId === 'kinii') {
                const p = e.selections[0]?.active;
                if (p) {
                    cursorInfo = {
                        uri: e.textEditor.document.uri.toString(),
                        line: p.line,
                        character: p.character,
                    };
                    // курсор ушёл / слово дописано — вернуть всё как есть
                    if (applyTimer) { clearTimeout(applyTimer); }
                    if (latestDiags.has(cursorInfo.uri)) {
                        applyTimer = setTimeout(() => {
                            applyTimer = null;
                            applyDiagnostics(vscode.Uri.parse(cursorInfo.uri));
                        }, 250);
                    }
                }
            }
        }),
        vscode.commands.registerCommand('kinii.restartServer', () => {
            stopServer();
            for (const uri of openDocs.keys()) collection.delete(vscode.Uri.parse(uri));
            openDocs.clear();
        }),
        // Форматировать открытый файл (Shift+Alt+F делает то же).
        vscode.commands.registerCommand('kinii.fmt', async () => {
            const doc = vscode.window.activeTextEditor?.document;
            if (!doc || doc.languageId !== 'kinii') { void 0; return; }
            const edits = await formatDocument(doc);
            const we = new vscode.WorkspaceEdit();
            we.set(doc.uri, edits);
            void vscode.workspace.applyEdit(we);
        }),
        // Богатый вид подсказок (описание + пример) — наша запись Ctrl+Space.
        // Если меню уже открыто, редактор игнорирует ретриггер — поэтому
        // сначала прячем виджет, затем открываем заново: провайдер обязан
        // вызваться и съесть флажок богатого вида.
        vscode.commands.registerCommand('kinii.richHints', async () => {
            richHintNext = Date.now();
            say('богатые подсказки: Ctrl+Space принят (пересоздаю меню)');
            await vscode.commands.executeCommand('hideSuggestWidget');
            await vscode.commands.executeCommand('editor.action.triggerSuggest');
        }),
        // Мягкое меню: перевести подтверждение подсказок на Tab/клик,
        // чтобы Enter продолжал печатать код и не принимал меню.
        vscode.commands.registerCommand('kinii.softHints', async () => {
            const cfg = vscode.workspace.getConfiguration('editor');
            const cur = cfg.get('acceptSuggestionOnEnter', 'on');
            const next = cur === 'off' ? 'on' : 'off';
            await cfg.update('acceptSuggestionOnEnter', next, vscode.ConfigurationTarget.Workspace);
            vscode.window.showInformationMessage(next === 'off'
                ? 'KINII: подсказки принимаются Tab/кликом; Enter печатает дальше'
                : 'KINII: подсказки снова принимаются и Enter тоже');
        }),
    );

    // Документы, открытые до активации расширения.
    for (const doc of vscode.workspace.textDocuments) {
        if (doc.languageId === 'kinii' && !doc.isClosed) didOpen(doc);
    }
}

function deactivate() {
    stopServer();
}

module.exports = { activate, deactivate };
