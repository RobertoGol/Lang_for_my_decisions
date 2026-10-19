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

// ---------- ответ ----------

void respond(const Json& id, const std::string& result_json) {
    std::string body = "{\"jsonrpc\":\"2.0\",\"id\":";
    if (id.kind == Json::Num) body += jint((long long)id.n);
    else if (id.kind == Json::Str) body += jstr(id.s);
    else body += "0";
    body += ",\"result\":" + result_json + "}";
    send(body);
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
            if (!uri.empty()) check_file(uri_to_path(uri), uri, text);
            continue;
        }

        if (name == "textDocument/didChange") {
            const Json* td = jf(*params, "textDocument");
            const Json* changes = jf(*params, "contentChanges");
            if (!td || !changes || changes->arr.empty()) continue;
            std::string uri = jf(*td, "uri") ? jf(*td, "uri")->s : "";
            const Json* tx = jf(changes->arr.back(), "text");
            if (tx && tx->kind == Json::Str && !uri.empty()) {
                check_file(uri_to_path(uri), uri, tx->s);
            }
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
