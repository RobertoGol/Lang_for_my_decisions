// LSP-сервер kinic: Language Server Protocol поверх stdio (JSON-RPC 2.0,
// фреймы Content-Length). Без внешних зависимостей: свой мини-JSON и
// свой цикл чтения. Поддерживает:
//   initialize / initialized / shutdown / exit
//   textDocument/didOpen, didChange (полный текст), didClose
//   textDocument/publishDiagnostics — после каждого изменения файла
//
// Синхронизация преднамеренно полная (не инкрементальная) — язык маленький,
// файлы маленькие: это дешевле и честнее, чем диффать текст в C++.

#include "kinii/diagnostic.h"
#include "kinii/driver.h"
#include "kinii/lexer.h"
#include "kinii/sema.h"

#include <cstdio>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace kinii {
namespace {

// ---------- мини-JSON ----------

struct Json {
    enum Kind { Null, Bool, Num, Str, Arr, Obj } kind = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Json> arr;
    std::vector<std::pair<std::string, Json>> obj;
};

struct Parser {
    const std::string& src;
    size_t p = 0;

    void ws() {
        while (p < src.size() &&
               (src[p] == ' ' || src[p] == '\t' || src[p] == '\n' || src[p] == '\r'))
            p++;
    }
    bool peek(char c) { return p < src.size() && src[p] == c; }

    Json value() {
        Json v;
        ws();
        if (p >= src.size()) return v;
        char c = src[p];
        if (c == '{') return object();
        if (c == '[') return array();
        if (c == '"') {
            v.kind = Json::Str;
            v.s = string();
            return v;
        }
        if (c == 't') { p += 4; v.kind = Json::Bool; v.b = true; return v; }
        if (c == 'f') { p += 5; v.kind = Json::Bool; v.b = false; return v; }
        if (c == 'n') { p += 4; return v; }
        return number();
    }
    Json object() {
        Json v;
        v.kind = Json::Obj;
        p++;
        ws();
        if (peek('}')) { p++; return v; }
        while (p < src.size()) {
            ws();
            Json k = value();
            ws();
            if (peek(':')) p++;
            Json val = value();
            v.obj.push_back({k.s, std::move(val)});
            ws();
            if (peek(',')) { p++; continue; }
            if (peek('}')) { p++; break; }
            break;
        }
        return v;
    }
    Json array() {
        Json v;
        v.kind = Json::Arr;
        p++;
        ws();
        if (peek(']')) { p++; return v; }
        while (p < src.size()) {
            v.arr.push_back(value());
            ws();
            if (peek(',')) { p++; continue; }
            if (peek(']')) { p++; break; }
            break;
        }
        return v;
    }
    std::string string() {
        std::string r;
        p++;
        while (p < src.size() && src[p] != '"') {
            char c = src[p];
            if (c == '\\' && p + 1 < src.size()) {
                char n = src[p + 1];
                if (n == 'n') { r += '\n'; p += 2; }
                else if (n == 't') { r += '\t'; p += 2; }
                else if (n == 'r') { p += 2; }
                else if (n == 'u' && p + 6 <= src.size()) {
                    unsigned code = 0;
                    try { code = std::stoul(src.substr(p + 2, 4), nullptr, 16); } catch (...) {}
                    if (code < 0x80) r += (char)code;
                    else if (code < 0x800) {
                        r += (char)(0xC0 | (code >> 6));
                        r += (char)(0x80 | (code & 0x3F));
                    } else {
                        r += (char)(0xE0 | (code >> 12));
                        r += (char)(0x80 | ((code >> 6) & 0x3F));
                        r += (char)(0x80 | (code & 0x3F));
                    }
                    p += 6;
                } else { r += n; p += 2; }
            } else { r += c; p++; }
        }
        if (p < src.size()) p++; // закрывающая "
        return r;
    }
    Json number() {
        size_t start = p;
        while (p < src.size() && ((src[p] >= '0' && src[p] <= '9') || src[p] == '-' ||
                                  src[p] == '+' || src[p] == '.' || src[p] == 'e' ||
                                  src[p] == 'E'))
            p++;
        Json v;
        if (p > start) {
            v.kind = Json::Num;
            try { v.n = std::stod(src.substr(start, p - start)); } catch (...) {}
        }
        return v;
    }
};

const Json* jf(const Json& obj, const std::string& key) {
    if (obj.kind != Json::Obj) return nullptr;
    for (const auto& kv : obj.obj)
        if (kv.first == key) return &kv.second;
    return nullptr;
}

std::string json_escape(const std::string& s) {
    std::string r;
    for (unsigned char c : s) {
        switch (c) {
            case '"': r += "\\\""; break;
            case '\\': r += "\\\\"; break;
            case '\n': r += "\\n"; break;
            case '\t': r += "\\t"; break;
            case '\r': break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof buf, "\\u%04x", c);
                    r += buf;
                } else {
                    r += (char)c;
                }
        }
    }
    return r;
}

// ---------- встроенная библиотека (описания для подсказок) ----------

// Описания из SPEC.md: то, что не объявляется в коде пользователя.
struct BuiltinDoc {
    std::string match;
    std::string text;
};

const std::vector<BuiltinDoc>& builtin_docs() {
    static const std::vector<BuiltinDoc> docs = {
        {"print",  "print(a, b, ...) — печатает аргументы подряд и переводит строку"},
        {"len",    "len(x) — длина списка/строки"},
        {"range",  "range(n) — диапазон для for/comp"},
        {"console.print", "console.print(a, b, ...) — то же, что print (имя для [SYS]-слоя)"},
        {"console.read_input", "console.read_input(\"подсказка\") — строка со стандартного ввода"},
        {"console.read_line",  "console.read_line() — строка со стандартного ввода"},
        {"sys.exit", "sys.exit(code) — завершить программу с кодом выхода"},
        {"sys.time.now", "sys.time.now() — время в миллисекундах (u64)"},
        {"sys.rnd.next", "sys.rnd.next(n) — случайное число 0..n-1"},
        {"u8",  "u8 — беззнаковое, 1 байт"},
        {"u16", "u16 — беззнаковое, 2 байта"},
        {"u32", "u32 — беззнаковое, 4 байта"},
        {"u64", "u64 — беззнаковое, 8 байт"},
        {"string", "string — строка (в бэкенде std::string)"},
        {"bool",   "bool — да/нет"},
        {"list",   "list<T> — динамический список"},
        {"node",   "node — узел: функции видны всему проекту; инструкции тела работают своим потоком"},
        {"isolate","isolate — изолят: отдельный поток; app_entry всегда дожидается его (join)"},
    };
    return docs;
}

std::string builtin_hover(const std::string& name) {
    // полный путь: "sys.time.now" — или последний сегмент: "sys", "u32"
    for (const auto& d : builtin_docs())
        if (name == d.match) return d.text;
    size_t dot = name.rfind('.');
    if (dot != std::string::npos) {
        std::string seg = name.substr(dot + 1);
        for (const auto& d : builtin_docs())
            if (seg == d.match) return d.text;
    }
    return "";
}

std::string jstr(const std::string& s) { return "\"" + json_escape(s) + "\""; }

std::string jint(long long n) {
    std::ostringstream ss;
    ss << n;
    return ss.str();
}

// ---------- фрейминг ----------

void send(const std::string& body) {
    std::cout << "Content-Length: " << body.size() << "\r\n\r\n" << body;
    std::cout.flush();
}

// ---------- позиции и пути ----------

std::string uri_to_path(const std::string& uri) {
    const std::string pref = "file:///";
    std::string p = uri;
#if defined(_WIN32)
    if (p.rfind(pref, 0) == 0) p = p.substr(pref.size());
    // drive-буква: /G:/proj → G:/proj
    if (p.size() > 2 && p[0] == '/' && p[2] == ':') p = p.substr(1);
#else
    if (p.rfind("file://", 0) == 0) p = p.substr(7);
#endif
    std::string out;
    for (size_t i = 0; i < p.size(); ++i) {
        if (p[i] == '%' && i + 2 < p.size()) {
            try {
                out += (char)std::stoul(p.substr(i + 1, 2), nullptr, 16);
                i += 2;
                continue;
            } catch (...) {}
        }
        out += p[i];
    }
    return out;
}

std::string path_to_uri(const std::string& path) {
    std::string r = "file:///";
    for (unsigned char c : path) {
        switch (c) {
            case ' ': r += "%20"; break;
            case '\"': r += "%22"; break;
            case '\\': r += "/"; break;
            default: r += (char)c;
        }
    }
    return r;
}

// LSP: строки 0-базные; символы — в UTF-16 единицах (у нас — кодпоинты, BMP совпадает).
std::string pos_of(const SourceLocation& l, bool end_marker) {
    long long line = (long long)l.line - 1;
    if (line < 0) line = 0;
    long long col = (long long)l.col - 1;
    if (col < 0) col = 0;
    if (end_marker) col += 1; // конец диапазона — сразу после символа
    return "{\"line\":" + jint(line) + ",\"character\":" + jint(col) + "}";
}

std::string range_of(const Span& sp) {
    return "{\"start\":" + pos_of(sp.begin, false) + ",\"end\":" + pos_of(sp.end, true) +
           "}";
}

std::string severity_of(Sev s) {
    switch (s) {
        case Sev::Error: return "1";
        case Sev::Warning: return "2";
        default: return "3";
    }
}

// ---------- публикация диагностик ----------

void publish(const DiagnosticEngine& diags, const std::string& uri) {
    std::string path = uri_to_path(uri);
    std::string items;
    bool first = true;
    for (const auto& d : diags.all()) {
        if (d.primary.file == kNoFile) continue;

        std::string dpath = diags.file_path(d.primary.file);
        if (dpath != path) continue; // ошибки другого файла уйдут в его uri отдельно
        // их publish выполнится при проверке того файла (didOpen/didChange)

        if (!first) items += ",";
        first = false;

        std::string item = "{\"range\":" + range_of(d.primary) +
                           ",\"severity\":" + severity_of(d.sev) +
                           ",\"code\":\"" + json_escape(d.code) +
                           "\",\"source\":\"kinic\",\"message\":" + jstr(d.message);

        // ссылки «объявлен в другом файле»
        std::string rels;
        for (const auto& l : d.labels) {
            if (l.span.file == kNoFile || l.span.file == d.primary.file) continue;
            std::string other = diags.file_path(l.span.file);
            if (other.empty()) continue;
            if (!rels.empty()) rels += ",";
            rels += "{\"location\":{\"uri\":" + jstr(path_to_uri(other)) +
                    ",\"range\":" + range_of(l.span) +
                    "},\"message\":" + jstr(l.text) + "}";
        }
        if (!rels.empty()) {
            item += ",\"relatedInformation\":[" + rels + "]";
        }
        item += "}";
        items += item;
    }
    send("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\","
         "\"params\":{\"uri\":" + jstr(uri) +
         ",\"diagnostics\":[" + items + "]}}");
}

// согласованный забег компиляции по одному файлу
void check_file(const std::string& path, const std::string& uri,
                const std::string& text) {
    DiagnosticEngine diags;
    SemaResult sema;
    sema.project_wide = true;
    Driver driver(diags, sema);
    driver.set_source_override(path, text);
    Span empty;
    driver.compile(path, empty);
    driver.finalize();
    publish(diags, uri);
}

// ---------- подсказка под строкой (textDocument/hover) ----------

// Ответ на запрос с id.
void respond(const Json& id, const std::string& result_json) {
    std::string body = "{\"jsonrpc\":\"2.0\",\"id\":";
    if (id.kind == Json::Num) body += jint((long long)id.n);
    else if (id.kind == Json::Str) body += jstr(id.s);
    else body += "0";
    body += ",\"result\":" + result_json + "}";
    send(body);
}

// Документы, о которых сервер знает (didOpen/didChange), — hover без диска.
std::map<std::string, std::string> g_open_docs;

std::string join_comma(const std::vector<std::string>& v) {
    std::string r;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) r += ", ";
        r += v[i];
    }
    return r;
}

std::string type_to_text(const TypeExpr* t) {
    if (!t) return "";
    switch (t->kind) {
        case TypeExpr::Named: return t->name;
        case TypeExpr::Cell:
            return (t->name.empty() ? "cell" : "cell:" + t->name);
        case TypeExpr::List:
            return "list<" + type_to_text(t->elem.get()) + ">";
        case TypeExpr::Array:
            return type_to_text(t->elem.get()) + "[" + std::to_string(t->array_len) + "]";
    }
    return "?";
}

// Объявление по имени поверх AST (в т.ч. функции, поднятые из node/isolate).
const Stmt* find_decl(const Stmt* root, const std::string& name) {
    if (!root) return nullptr;
    for (const auto& s : root->body) {
        if (s && s->name == name) return s.get();
        if (s && (s->kind == Stmt::NodeDecl || s->kind == Stmt::IsolateDecl) &&
            s->block) {
            for (const auto& inner : s->block->body)
                if (inner && inner->kind == Stmt::FnDecl && inner->name == name)
                    return inner.get();
        }
    }
    return nullptr;
}

std::string render_fn_sig(const Stmt& s) {
    std::string r = s.is_pub ? "pub fn " : "fn ";
    r += s.name + "(";
    for (size_t i = 0; i < s.params.size(); ++i) {
        if (i) r += ", ";
        r += s.params[i].name;
        if (s.params[i].type) r += ": " + type_to_text(s.params[i].type.get());
    }
    r += ")";
    if (s.ret) r += " -> " + type_to_text(s.ret.get());
    return r;
}

std::string render_struct_sig(const Stmt& s) {
    std::string r = "struct " + s.name + " { ";
    for (size_t i = 0; i < s.fields.size(); ++i) {
        if (i) r += "; ";
        r += s.fields[i].name;
        if (s.fields[i].type) r += ": " + type_to_text(s.fields[i].type.get());
    }
    r += (s.fields.empty() ? "}" : " }");
    return r;
}

std::string render_enum_sig(const Stmt& s) {
    return "enum " + s.name + " { " + join_comma(s.enum_items) + " }";
}

// Имя файла без каталога — для «объявлен в файле X».
std::string file_name_of(const std::string& path) {
    size_t s = path.find_last_of('/');
    return s == std::string::npos ? path : path.substr(s + 1);
}

// Основной расчёт подсказки: имя под курсором → что оно такое → markdown.
// Если сказать нечего — честный null.
void hover_at(const std::string& path, const std::string& uri,
              const std::string& text, long long line, long long col,
              const Json& id) {
    DiagnosticEngine diags;
    SemaResult sema;
    sema.project_wide = true;
    Driver driver(diags, sema);
    driver.set_source_override(path, text);
    Span empty;
    FileId hfid = driver.compile(path, empty);
    driver.finalize();
    if (hfid == kNoFile) { respond(id, "null"); return; }
    publish(diags, uri); // диагностики не отстают и при подсказке

    // Токен под курсором (позиции LSP 0-базные → KINII 1-базные).
    uint32_t ln = (uint32_t)(line + 1);
    uint32_t cl = (uint32_t)(col + 1);

    DiagnosticEngine lex_diags; // ошибки лексера тихо: их уже показал check
    FileId lex_fid = lex_diags.add_file(path, text);
    std::vector<Token> toks = lex_file(lex_fid, text, lex_diags);

    long long hit = -1;
    for (size_t i = 0; i < toks.size(); ++i) {
        const Token& t = toks[i];
        if (t.type == Tok::Eof) break;
        bool after_begin =
            (t.begin.line < ln) || (t.begin.line == ln && t.begin.col <= cl);
        bool before_end =
            (t.end.line > ln) || (t.end.line == ln && t.end.col >= cl);
        if (after_begin && before_end) { hit = (long long)i; break; }
    }
    if (hit < 0 || toks[(size_t)hit].type != Tok::Ident) {
        respond(id, "null");
        return;
    }
    const std::string name = toks[(size_t)hit].text;

    // Полный путь через точки: hover на "now" у sys.time.now → "sys.time.now".
    std::string dotted = name;
    {
        size_t a = (size_t)hit;
        while (a >= 2 && toks[a - 1].type == Tok::Dot &&
               toks[a - 2].type == Tok::Ident)
            a -= 2;
        std::string chain = toks[a].text;
        size_t k = a;
        while (k < (size_t)hit) {
            k += 2; // Dot, Ident
            chain += "." + toks[k].text;
        }
        size_t r = (size_t)hit + 1;
        while (r + 1 < toks.size() && toks[r].type == Tok::Dot &&
               toks[r + 1].type == Tok::Ident) {
            chain += "." + toks[r + 1].text;
            r += 2;
        }
        if (chain.size() > dotted.size()) dotted = chain;
    }

    // 1) Встроенная библиотека (полный путь, затем просто имя).
    std::string info = builtin_hover(dotted);
    std::string decl_note;
    if (info.empty()) {
        // 2) Таблица символов проекта.
        const Symbol* sym = nullptr;
        const FileSema* decl_file = nullptr;
        if (hfid < sema.files.size()) {
            auto it = sema.files[hfid].top_decls.find(name);
            if (it != sema.files[hfid].top_decls.end()) {
                sym = &it->second;
                decl_file = &sema.files[hfid];
            }
        }
        if (!sym) {
            auto it = sema.global.find(name);
            if (it != sema.global.end()) {
                sym = &it->second;
                if (sym->file < sema.files.size())
                    decl_file = &sema.files[sym->file];
            }
        }
        if (sym) {
            if (sym->kind == Symbol::Field) {
                // в таблице поля лежат как "Point.x"
                size_t d = sym->name.find('.');
                std::string owner =
                    d == std::string::npos ? "" : sym->name.substr(0, d);
                info = "поле '" + name + "' структуры " + owner +
                       (sym->type_text.empty() ? "" : " — " + sym->type_text);
            } else {
                const Stmt* def = find_decl(driver.ast_of(sym->file), name);
                switch (sym->kind) {
                    case Symbol::Fn:
                        if (def && def->kind == Stmt::FnDecl) {
                            info = render_fn_sig(*def);
                        } else {
                            info = "fn " + name + "()";
                            if (!sym->type_text.empty() && sym->type_text != "void")
                                info += " -> " + sym->type_text;
                        }
                        break;
                    case Symbol::Struct:
                        if (sym->type_text == "node" || sym->type_text == "isolate") {
                            for (const auto& d : builtin_docs())
                                if (d.match == sym->type_text) info = d.text;
                            info = sym->type_text + " " + name +
                                   (info.empty() ? "" : " — " + info);
                        } else {
                            info = def ? render_struct_sig(*def) : "struct " + name;
                        }
                        break;
                    case Symbol::Enum:
                        info = def ? render_enum_sig(*def) : "enum " + name;
                        break;
                    case Symbol::Var:
                    case Symbol::Const:
                        info = (sym->is_const ? "const " : "let ") + name +
                               (sym->type_text.empty()
                                    ? " (тип выводится сам)"
                                    : (": " + sym->type_text));
                        break;
                    default:
                        info = name + (sym->type_text.empty() ? "" : " — " + sym->type_text);
                }
            }
            if (sym->span.file != kNoFile && sym->span.begin.line) {
                decl_note = sym->file == hfid
                                ? "объявлен в этом файле, строка " +
                                      std::to_string((unsigned)sym->span.begin.line)
                                : "объявлен в файле '" +
                                      (decl_file ? file_name_of(decl_file->path)
                                                 : "") +
                                      "', строка " +
                                      std::to_string((unsigned)sym->span.begin.line);
            }
        }
    }

    if (info.empty()) { respond(id, "null"); return; }

    std::string md = "```kinii\n" + info + "\n```";
    if (!decl_note.empty()) md += "\n\n" + decl_note;
    respond(id,
            "{\"contents\":{\"kind\":\"markdown\",\"value\":\"" +
                json_escape(md) + "\"}}");
}

// ---------- цикл ----------

bool read_headers(size_t& content_len) {
    content_len = 0;
    std::string key;
    while (std::cin) {
        std::string line;
        int c;
        while ((c = std::cin.get()) != EOF) {
            if (c == '\n') break;
            if (c != '\r') line += (char)c;
        }
        if (!std::cin) return false;
        if (line.empty()) return true; // конец заголовков
        size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string name = line.substr(0, colon);
            std::string val = line.substr(colon + 1);
            // убираем пробелы в начале
            size_t k = val.find_first_not_of(' ');
            if (k != std::string::npos) val = val.substr(k);
            if (name == "Content-Length")
                try { content_len = std::stoul(val); } catch (...) { return false; }
        }
    }
    return false;
}

int run() {
    // На Windows stdio по умолчанию в текстовом режиме: он портит \r\n из
    // фреймов Content-Length. Переводим stdin/stdout в бинарный режим.
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    std::cerr << "kinic lsp: готов\n";

    while (true) {
        size_t len = 0;
        if (!read_headers(len)) return 0;
        std::string body(len, '\0');
        if (len) std::cin.read(&body[0], (std::streamsize)len);
        if (!std::cin) return 0;

        Parser ps{body};
        Json m = ps.value();
        if (m.kind != Json::Obj) continue;

        const Json* method = jf(m, "method");
        const Json* idv = jf(m, "id");

        // ответные сообщения клиента игнорируем
        if (!method) continue;
        const std::string& name = method->s;

        if (name == "initialize") {
            const Json* id = idv;
            if (id)
                respond(*id,
                        "{\"capabilities\":{"
                        "\"textDocumentSync\":{\"openClose\":true,\"change\":1}"
                        "},\"serverInfo\":{\"name\":\"kinic\",\"version\":\"0.1.0\"}}");
            continue;
        }
        if (name == "initialized") continue;
        if (name == "shutdown") {
            if (idv) respond(*idv, "null");
            continue;
        }
        if (name == "exit") return 0;

        const Json* params = jf(m, "params");
        if (!params) continue;

        if (name == "textDocument/didOpen") {
            const Json* td = jf(*params, "textDocument");
            if (!td) continue;
            std::string uri = td->kind == Json::Obj ? jf(*td, "uri") ? jf(*td, "uri")->s : ""
                                                           : "";
            std::string text;
            const Json* tx = jf(*td, "text");
            if (tx && tx->kind == Json::Str) text = tx->s;
            if (!uri.empty()) {
                g_open_docs[uri] = text;
                check_file(uri_to_path(uri), uri, text);
            }
            continue;
        }

        if (name == "textDocument/didChange") {
            const Json* td = jf(*params, "textDocument");
            const Json* changes = jf(*params, "contentChanges");
            if (!td || !changes || changes->arr.empty()) continue;
            std::string uri = jf(*td, "uri") ? jf(*td, "uri")->s : "";
            const Json* tx = jf(changes->arr.back(), "text");
            if (tx && tx->kind == Json::Str && !uri.empty()) {
                g_open_docs[uri] = tx->s;
                check_file(uri_to_path(uri), uri, tx->s);
            }
            continue;
        }

        if (name == "textDocument/hover") {
            const Json* td = jf(*params, "textDocument");
            const Json* pos = jf(*params, "position");
            const Json* id = idv;
            if (!id) continue; // hover — всегда запрос, id обязателен
            std::string uri = td && td->kind == Json::Obj
                                  ? (jf(*td, "uri") ? jf(*td, "uri")->s : "")
                                  : "";
            auto doc = g_open_docs.find(uri);
            std::string text =
                doc != g_open_docs.end() ? doc->second : std::string();
            if (text.empty() || !pos || pos->kind != Json::Obj) {
                respond(*id, "null");
                continue;
            }
            long long line = 0, chr = 0;
            const Json* jline = jf(*pos, "line");
            const Json* jchar = jf(*pos, "character");
            if (jline && jline->kind == Json::Num) line = (long long)jline->n;
            if (jchar && jchar->kind == Json::Num) chr = (long long)jchar->n;
            hover_at(uri_to_path(uri), uri, text, line, chr, *id);
            continue;
        }

        if (name == "textDocument/didClose") {
            // подчистим диагностики редактора для закрытого файла
            const Json* td = jf(*params, "textDocument");
            std::string uri = td ? (jf(*td, "uri") ? jf(*td, "uri")->s : "") : "";
            if (!uri.empty())
                send("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/"
                     "publishDiagnostics\",\"params\":{\"uri\":" + jstr(uri) +
                     ",\"diagnostics\":[]}}");
            continue;
        }

        // всё остальное не критично: тишина
    }
}

} // namespace

int cmd_lsp() { return run(); }

} // namespace kinii
