const vscode = require('vscode');
const { exec } = require('child_process');
const path = require('path');

function activate(context) {
    const diagnosticCollection = vscode.languages.createDiagnosticCollection('voidcore');
    context.subscriptions.push(diagnosticCollection);

    function checkDocument(document) {
        if (document.languageId !== 'voidcore') return;

        const filePath = document.fileName;
        // Путь к твоему void_win.exe (лежит в корне проекта)
        const compilerPath = path.join(vscode.workspace.rootPath, 'void_win.exe');

        // Запуск: void_win.exe --check "путь_к_файлу"
        exec(`"${compilerPath}" --check "${filePath}"`, (error, stdout, stderr) => {
            diagnosticCollection.clear();
            if (stdout) {
                try {
                    // Ожидаем от компилятора JSON: {"line":10, "col":15, "missing":"}", "prev_word":"Vault"}
                    const err = JSON.parse(stdout);
                    reportSmartError(document, err);
                } catch (e) {
                    // Если компилятор выдал просто текст, парсим его регуляркой
                    const match = stdout.match(/line (\d+), col (\d+): missing (.+) after (.+)/);
                    if (match) {
                        reportSmartError(document, {
                            line: parseInt(match[1]),
                            column: parseInt(match[2]),
                            missing: match[3],
                            prev_word: match[4]
                        });
                    }
                }
            }
        });
    }

    function reportSmartError(document, errorData) {
        const line = errorData.line - 1;
        const col = Math.max(0, errorData.column - 1);
        
        const message = `❌ [VOID-CORE ERROR]\n` +
                        `Пропущен символ: [ ${errorData.missing} ]\n` +
                        `Место: Сразу после слова "${errorData.prev_word}"\n` +
                        `Совет: Проверьте синтаксис в этой области.`;

        const range = new vscode.Range(line, col, line, col + 1);
        const diagnostic = new vscode.Diagnostic(range, message, vscode.DiagnosticSeverity.Error);
        diagnosticCollection.set(document.uri, [diagnostic]);
    }

    context.subscriptions.push(vscode.workspace.onDidSaveTextDocument(checkDocument));
    context.subscriptions.push(vscode.workspace.onDidOpenTextDocument(checkDocument));
}

exports.activate = activate;
