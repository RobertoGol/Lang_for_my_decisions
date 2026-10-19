// Проверка пользовательских хоткеев VSCodium как jsonc-parser (комментарии +
// завершающие запятые разрешены, как в VS Code).
const fs = require('fs');
const path = require('path');
const p = path.join(process.env.APPDATA, 'VSCodium', 'User', 'keybindings.json');
const raw = fs.readFileSync(p, 'utf8');
let s = raw.replace(/^[ \t]*\/\/.*$/gm, '');
s = s.replace(/,(\s*[}\]])/g, '$1');
try {
    const arr = JSON.parse(s);
    console.log('.keybindings OK, правил в файле:', arr.length);
} catch (e) {
    console.log('FAIL:', e.message);
    process.exit(1);
}
raw.split('\n').forEach((line, i) => {
    const t = line.trim();
    if (t.includes('acceptSelectedSuggestion') || t.includes('Python') ||
        t.includes('ctrl+space')) {
        console.log((i + 1) + ': ' + t);
    }
});
