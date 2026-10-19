# Smoke-тест LSP-сервера kinic:
#   initialize -> didOpen (с ошибкой) -> didChange (исправили) -> shutdown -> exit
# Ожидаем: дубликата нет; на didOpen одна diagnostic по неизвестному имени,
# после didChange диагностики чистые.

$ErrorActionPreference = 'Stop'
$kinic = Join-Path $PSScriptRoot '..\compiler\build\kinic.exe'

function Frame([string]$body) {
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($body)
    $head = [System.Text.Encoding]::ASCII.GetBytes(
        "Content-Length: $($bytes.Length)`r`n`r`n")
    $ms = New-Object System.IO.MemoryStream
    $ms.Write($head, 0, $head.Length)
    $ms.Write($bytes, 0, $bytes.Length)
    return $ms.ToArray()
}

# Пути uri только с '/', как их присылает VS Code.
$uri = 'file:///G:/Project/Game/KINII/examples/lsp_smoke_tmp.vc'

$msgs = @(
    '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"rootUri":"file:///G:/Project/Game/KINII"}}',
    '{"jsonrpc":"2.0","method":"initialized","params":{}}',
    ('{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"' +
     $uri + '","languageId":"kinii","version":1,"text":"let y: int = no_such_name;\nconst PI = 3;\n"}}}'),
    ('{"jsonrpc":"2.0","method":"textDocument/didChange","params":{"textDocument":{"uri":"' +
     $uri + '","version":2},"contentChanges":[{"text":"let y = 5;\nconst PI = 3;\n"}]}}'),
    ('{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"' +
     $uri + '","languageId":"kinii","version":3,"text":"import \"util.ctos\";\nfn double_of(x: u32) -> u32 { return x; }\n"}}}'),
    ('{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"' +
     $uri + '","languageId":"kinii","version":4,"text":"import \"util.ctos\";\nlet n = double_of(21);\nlet tms2 = sys.time.now();\n"}}}'),
    '{"jsonrpc":"2.0","id":2,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///G:/Project/Game/KINII/examples/lsp_smoke_tmp.vc"},"position":{"line":1,"character":10}}}',
    '{"jsonrpc":"2.0","id":3,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///G:/Project/Game/KINII/examples/lsp_smoke_tmp.vc"},"position":{"line":2,"character":16}}}',
    '{"jsonrpc":"2.0","id":4,"method":"shutdown"}',
    '{"jsonrpc":"2.0","method":"exit"}'
)

$in = [byte[]]@()
foreach ($m in $msgs) { $in += Frame $m }

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $kinic
$psi.Arguments = 'lsp'
$psi.RedirectStandardInput = $true
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$psi.UseShellExecute = $false
$psi.StandardOutputEncoding = [System.Text.Encoding]::UTF8
$psi.StandardErrorEncoding = [System.Text.Encoding]::UTF8

$p = [System.Diagnostics.Process]::Start($psi)
$p.StandardInput.BaseStream.Write($in, 0, $in.Length)
$p.StandardInput.BaseStream.Flush()
$p.StandardInput.Close()

$out = $p.StandardOutput.ReadToEnd()
$err = $p.StandardError.ReadToEnd()
$p.WaitForExit()

'--- stderr ---'
$err
'--- stdout (сообщения по одному) ---'
$out -split "(?=Content-Length:)" | Where-Object { $_.Trim() } | ForEach-Object {
    $body = $_ -replace '(?s)^Content-Length: \d+\r\n\r\n', ''
    $body
    ''
}
"exit code: $($p.ExitCode)"
