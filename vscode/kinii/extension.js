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

// Появляется, пока пишешь; работает только как подсказка — можно полностью
// игнорировать и продолжать печатать. Принять — мышкой или Tab.
// Идея: gallery «примеров команд» языка + ключевые слова + системные вызовы.
const HINTS = [
    ['let', 'let имя = значение;', 'переменная: let счётчик = 0;'],
    ['const', 'const ИМЯ = значение;', 'константа, которую нельзя менять: const ПРЕДЕЛ = 100;'],
    ['fn', 'fn имя(а: u32) -> u32 {\n    return $1;\n}', 'функция: fn двойня(х: u32) -> u32 { return х * 2; }'],
    ['struct', 'struct Имя {\n    поле: u32;\n}', 'структура (поля через «;»): struct Точка { х: u32; у: u32; }'],
    ['enum', 'enum Имя { А, Б, В }', 'перечисление: enum Цвет { Красный, Зелёный }'],
    ['if', 'if (условие) {\n    $1\n} else {\n    $2\n}', 'ветвление: if (r == 4) { sys.exit(4); }'],
    ['while', 'while (условие) {\n    $1\n}', 'цикл с условием: while (n > 0) { n = n - 1; }'],
    ['loop', 'loop {\n    $1\n}', 'бесконечный цикл (выход — break)'],
    ['for', 'for x in xs {\n    $1\n}', 'цикл по коллекции: for x in элементы { print(x); }'],
    ['decide', 'decide(значение) {\n    weight(вариант) => {\n        $1\n    }\n}', 'честный switch: decide(cmd) { weight("go") => {...} }'],
    ['print', 'print(${1:a}, ${2:b});', 'печать в консоль: print("привет:", имя);'],
    ['return', 'return ${1:значение};', 'возврат из функции'],
    ['break', 'break;', 'выход из ближайшего цикла (работает и внутри decide)'],
    ['continue', 'continue;', 'следующий шаг цикла'],
    ['node', 'node Имя {\n    fn функция() -> u32 {\n        return ${1:3};\n    }\n\n    $2\n}', 'узел: свои функции и инструкции-поток, стартует при запуске'],
    ['isolate', 'isolate Имя {\n    ${1:let warmed = 0; ...}\n}', 'изолят: задача, живущая параллельно с app_entry'],
    ['app_entry', 'app_entry {\n    $1\n}', 'точка входа программы'],
    ['import', 'import "${1:файл.ctos}";', 'в явном виде — необязательно: с project.kinii всё видно'],
    ['sys.exit', 'sys.exit(${1:4});', 'честный код выхода (как в C): 0 — успех'],
    ['sys.time.now', 'sys.time.now()', 'время в миллисекундах'],
    ['sys.rnd.next', 'sys.rnd.next(${1:максимум})', 'случайное число до максимума'],
    ['console.print', 'console.print(${1:a}, ${2:b});', 'то же, что print — имя для [SYS]-слоя'],
    ['console.read_input', 'console.read_input()', 'прочитать строку с клавиатуры'],
];

function provideSoftHints(doc, pos) {
    // Выключаемо настройкой kinii.hints.
    if (!vscode.workspace.getConfiguration('kinii').get('hints', true)) return null;
    // Не подсказываем внутри комментария.
    const before = doc.getText(new vscode.Range(new vscode.Position(pos.line, 0), pos));
    if (before.lastIndexOf('//') >= 0) return null;
    return HINTS.map(([label, snippet, detail]) => {
        const it = new vscode.CompletionItem(label, vscode.CompletionItemKind.Snippet);
        it.insertText = new vscode.SnippetString(snippet);
        it.detail = 'KINII: ' + detail;
        it.sortText = '0' + label; // примеры — выше обычных имен из файла
        return it;
    });
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

    context.subscriptions.push(
        vscode.workspace.onDidOpenTextDocument((d) => { if (d.languageId === 'kinii') didOpen(d); }),
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
        vscode.languages.registerCompletionItemProvider('kinii', {
            provideCompletionItems: provideSoftHints,
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
