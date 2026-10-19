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
        collection.set(uri, diags);
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
    collection.delete(doc.uri);
    sendServer({
        jsonrpc: '2.0', method: 'textDocument/didClose',
        params: { textDocument: { uri: doc.uri.toString() } },
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
            // отвечает сигнатурой и местом объявления. Никаких выпадающих
            // списков автодополнения — они не нужны.
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
        vscode.workspace.onDidChangeTextDocument((e) => {
            if (e.document.languageId === 'kinii') didChange(e.document);
        }),
        vscode.workspace.onDidCloseTextDocument(didClose),
        vscode.commands.registerCommand('kinii.restartServer', () => {
            stopServer();
            for (const uri of openDocs.keys()) collection.delete(vscode.Uri.parse(uri));
            openDocs.clear();
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
