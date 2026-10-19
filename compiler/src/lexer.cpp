// Лексер KINII.
// Особенности синтаксиса, которые он должен знать:
//   &-        — мост [LOGIC] -> [SYS]
//   <?sql .. ?> — встраиваемый SQL (один токен с телом запроса)
//   #if/#endif — директивы компилятора
//   0x1F      — шестнадцатеричные числа
// Колонки считаются в символах (кодпоинтах), чтобы кириллица не сбивала позиции.

#include "kinii/lexer.h"

#include <string_view>
#include <unordered_map>

namespace kinii {

namespace {

bool is_cont(unsigned char c) { return (c & 0xC0) == 0x80; }
bool ident_start(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
bool ident_cont(char c) { return ident_start(c) || (c >= '0' && c <= '9'); }
bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_hex(char c) { return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

const std::unordered_map<std::string, Tok> kKeywords = {
    {"let", Tok::KwLet},         {"const", Tok::KwConst},
    {"fn", Tok::KwFn},           {"struct", Tok::KwStruct},
    {"enum", Tok::KwEnum},       {"node", Tok::KwNode},
    {"isolate", Tok::KwIsolate}, {"interface", Tok::KwInterface},
    {"page", Tok::KwPage},       {"app_entry", Tok::KwAppEntry},
    {"spawn", Tok::KwSpawn},     {"import", Tok::KwImport},
    {"from", Tok::KwFrom},       {"as", Tok::KwAs},
    {"pub", Tok::KwPub},         {"priv", Tok::KwPriv},
    {"async", Tok::KwAsync},     {"await", Tok::KwAwait},
    {"entry", Tok::KwEntry},     {"handler", Tok::KwHandler},
    {"if", Tok::KwIf},           {"else", Tok::KwElse},
    {"while", Tok::KwWhile},     {"loop", Tok::KwLoop},
    {"for", Tok::KwFor},         {"in", Tok::KwIn},
    {"return", Tok::KwReturn},   {"move", Tok::KwMove},
    {"decide", Tok::KwDecide},   {"weight", Tok::KwWeight},
    {"break", Tok::KwBreak},     {"continue", Tok::KwContinue},
    {"true", Tok::KwTrue},       {"false", Tok::KwFalse},
    {"cell", Tok::KwCell},       {"type", Tok::KwType},
    {"operator", Tok::KwOperator}, {"define", Tok::KwDefine},
    {"tag", Tok::KwTag},         {"asm", Tok::KwAsm},
    // типы
    {"u8", Tok::TyU8},   {"u16", Tok::TyU16}, {"u32", Tok::TyU32},
    {"u64", Tok::TyU64}, {"string", Tok::TyString}, {"bool", Tok::TyBool},
    {"ptr", Tok::TyPtr}, {"v_word", Tok::TyVWord}, {"v_addr", Tok::TyVAddr},
    {"list", Tok::TyList},
};

struct Lexer {
    FileId fid;
    const std::string& src;
    DiagnosticEngine& dg;
    size_t pos = 0;
    uint32_t line = 1, col = 1;

    bool at_end() const { return pos >= src.size(); }
    char cur(size_t d = 0) const { return pos + d < src.size() ? src[pos + d] : '\0'; }

    void bump() {
        if (at_end()) return;
        unsigned char c = (unsigned char)src[pos];
        if (c == '\n') {
            line++;
            col = 1;
        } else if (!is_cont(c)) {
            col++; // байт продолжения UTF-8 не начинает новый символ
        }
        pos++;
    }

    SourceLocation here() const { return {line, col, (uint32_t)pos}; }

    Token make(Tok t, SourceLocation b, std::string text, std::string value = "") {
        Token tk;
        tk.type = t;
        tk.text = std::move(text);
        tk.value = std::move(value);
        tk.begin = b;
        tk.end = here();
        return tk;
    }

    Span span_of(SourceLocation b) const {
        Span s;
        s.file = fid;
        s.begin = b;
        s.end = here();
        return s;
    }
};

// Читает идентификатор/ключевое слово, начиная с уже проверенного первого символа.
Token lex_ident(Lexer& L) {
    SourceLocation b = L.here();
    size_t start = L.pos;
    L.bump(); // первый символ
    while (ident_cont(L.cur())) L.bump();
    std::string text = L.src.substr(start, L.pos - start);
    auto it = kKeywords.find(text);
    Tok t = (it != kKeywords.end()) ? it->second : Tok::Ident;
    return L.make(t, b, std::move(text));
}

Token lex_number(Lexer& L, DiagnosticEngine& dg) {
    SourceLocation b = L.here();
    size_t start = L.pos;

    if (L.cur() == '0' && (L.cur(1) == 'x' || L.cur(1) == 'X')) {
        L.bump();
        L.bump();
        size_t hex_start = L.pos;
        while (is_hex(L.cur())) L.bump();
        if (L.pos == hex_start) {
            // Цифр после 0x нет: съедаем прилипшие буквы, чтобы подсветить всё сразу.
            while (ident_cont(L.cur())) L.bump();
            std::string full = L.src.substr(start, L.pos - start);
            Span sp = L.span_of(b);
            dg.error("K0004", "после '0x' ожидаются шестнадцатеричные цифры: '" + full + "'", sp)
                .hint("в 0x-числах допустимы только 0-9 и a-f/A-F")
                .fix("как должно выглядеть:",
                     "let ptr addr = 0x11000000;")
                .emit();
            return L.make(Tok::Unknown, b, std::move(full));
        }
    } else {
        while (is_digit(L.cur())) L.bump();
    }

    // Дробные числа пока не поддерживаются — объясняем это сразу.
    if (L.cur() == '.' && is_digit(L.cur(1))) {
        Span sp = L.span_of(b);
        dg.error("K0007", "дробные числа пока не поддерживаются в KINII", sp)
            .hint("для работы с дробями используйте целые с масштабированием: 1500 вместо 1.5")
            .emit();
        while (is_digit(L.cur())) L.bump();
    }

    // 50x — число с прилипшими буквами: частая опечатка.
    if (ident_start(L.cur())) {
        while (ident_cont(L.cur())) L.bump();
        std::string full = L.src.substr(start, L.pos - start);
        Span sp = L.span_of(b);
        dg.error("K0003", "число не может содержать буквы: '" + full + "'", sp)
            .hint("если это шестнадцатеричное число — добавьте префикс 0x")
            .hint("если это два разных токена — разделите их пробелом")
            .fix("как должно выглядеть:",
                 "let u32 count = 50;      // десятичное\n"
                 "let ptr addr = 0x50FF;   // шестнадцатеричное")
            .emit();
        return L.make(Tok::Unknown, b, std::move(full));
    }

    std::string text = L.src.substr(start, L.pos - start);
    return L.make(Tok::Number, b, std::move(text));
}

Token lex_string(Lexer& L, DiagnosticEngine& dg) {
    SourceLocation b = L.here();
    L.bump(); // открывающая кавычка
    std::string value;
    bool closed = false;
    while (!L.at_end()) {
        char c = L.cur();
        if (c == '"') {
            L.bump();
            closed = true;
            break;
        }
        if (c == '\n') break; // строки не переходят на новую строку
        if (c == '\\') {
            L.bump();
            char e = L.cur();
            switch (e) {
                case 'n': value.push_back('\n'); L.bump(); break;
                case 't': value.push_back('\t'); L.bump(); break;
                case 'r': value.push_back('\r'); L.bump(); break;
                case '0': value.push_back('\0'); L.bump(); break;
                case '\\': value.push_back('\\'); L.bump(); break;
                case '"': value.push_back('"'); L.bump(); break;
                default: {
                    Span sp = L.span_of(L.here());
                    dg.warning("K0008", "неизвестная escape-последовательность '\\" +
                                             std::string(1, e) + "' — оставлена как есть",
                               sp)
                        .hint("известные: \\n \\t \\r \\0 \\\\ \\\"")
                        .emit();
                    value.push_back('\\');
                    value.push_back(e);
                    L.bump();
                    break;
                }
            }
            continue;
        }
        value.push_back(c);
        L.bump();
    }

    std::string text = L.src.substr(b.offset, L.pos - b.offset);
    if (!closed) {
        Span sp = L.span_of(b);
        dg.error("K0005", "строка не закрыта — не хватает кавычки \"", sp)
            .hint("строки в KINII не переносятся на новую строку; используйте \\n")
            .fix("как должно выглядеть:",
                 "let string msg = \"привет, мир\";")
            .emit();
    }
    return L.make(Tok::String, b, std::move(text), std::move(value));
}

// <?sql SELECT ...; ?> — один токен с телом запроса в value.
Token lex_sql_tag(Lexer& L, DiagnosticEngine& dg) {
    SourceLocation b = L.here();
    L.bump(); // <
    L.bump(); // ?
    // после <? ожидаем 'sql' (регистр не важен)
    size_t word_start = L.pos;
    while (ident_cont(L.cur())) L.bump();
    std::string word = L.src.substr(word_start, L.pos - word_start);
    for (auto& ch : word) ch = (char)tolower((unsigned char)ch);

    if (word != "sql") {
        Span sp = L.span_of(b);
        dg.error("K0009", "неизвестный встроенный тег '<?" + word + "?>' — существует только '<?sql ?>'", sp)
            .fix("как должно выглядеть:",
                 "<?sql SELECT name FROM ghost_table; ?>")
            .emit();
    }

    // ищем закрывающий ?>
    size_t body_start = L.pos;
    size_t close = std::string::npos;
    while (!L.at_end()) {
        if (L.cur() == '?' && L.cur(1) == '>') {
            close = L.pos;
            break;
        }
        L.bump();
    }
    std::string body = L.src.substr(body_start, (close == std::string::npos ? L.pos : close) - body_start);

    if (close == std::string::npos) {
        Span sp = L.span_of(b);
        dg.error("K0006", "SQL-тег не закрыт — не хватает '?>'", sp)
            .fix("как должно выглядеть:",
                 "<?sql SELECT name, status FROM ghost_table; ?>")
            .emit();
    } else {
        L.bump(); // ?
        L.bump(); // >
    }

    // убираем пробелы по краям тела
    size_t b0 = body.find_first_not_of(" \t\r\n");
    size_t b1 = body.find_last_not_of(" \t\r\n");
    std::string trimmed = (b0 == std::string::npos) ? "" : body.substr(b0, b1 - b0 + 1);

    std::string text = L.src.substr(b.offset, L.pos - b.offset);
    return L.make(Tok::SqlTag, b, std::move(text), std::move(trimmed));
}

} // namespace

std::vector<Token> lex_file(FileId fid, const std::string& src, DiagnosticEngine& diags) {
    Lexer L{fid, src, diags};
    std::vector<Token> out;

    // Пропускаем BOM, если редактор его добавил.
    if (src.size() >= 3 && (unsigned char)src[0] == 0xEF && (unsigned char)src[1] == 0xBB &&
        (unsigned char)src[2] == 0xBF) {
        L.bump();
        L.bump();
        L.bump();
    }

    while (true) {
        // --- пропускаем пробелы и комментарии ---
        while (!L.at_end()) {
            char c = L.cur();
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                L.bump();
            } else if (c == '/' && L.cur(1) == '/') {
                while (!L.at_end() && L.cur() != '\n') L.bump();
            } else if (c == '/' && L.cur(1) == '*') {
                SourceLocation cb = L.here();
                L.bump();
                L.bump();
                bool closed = false;
                while (!L.at_end()) {
                    if (L.cur() == '*' && L.cur(1) == '/') {
                        L.bump();
                        L.bump();
                        closed = true;
                        break;
                    }
                    L.bump();
                }
                if (!closed) {
                    Span sp = L.span_of(cb);
                    diags.error("K0010", "блок-комментарий не закрыт — не хватает '*/'", sp)
                        .fix("как должно выглядеть:", "/* это комментарий */")
                        .emit();
                }
            } else {
                break;
            }
        }

        if (L.at_end()) {
            out.push_back(L.make(Tok::Eof, L.here(), ""));
            break;
        }

        SourceLocation b = L.here();
        char c = L.cur();

        // --- идентификаторы и ключевые слова ---
        if (ident_start(c)) {
            out.push_back(lex_ident(L));
            continue;
        }

        // --- числа ---
        if (is_digit(c)) {
            out.push_back(lex_number(L, diags));
            continue;
        }

        // --- строки ---
        if (c == '"') {
            out.push_back(lex_string(L, diags));
            continue;
        }

        // --- SQL-тег <?sql ... ?> ---
        if (c == '<' && L.cur(1) == '?') {
            out.push_back(lex_sql_tag(L, diags));
            continue;
        }

        // --- мост &- ---
        if (c == '&' && L.cur(1) == '-') {
            L.bump();
            L.bump();
            out.push_back(L.make(Tok::Bridge, b, "&-"));
            continue;
        }

        // --- директивы #if / #endif ---
        if (c == '#' && ident_start(L.cur(1))) {
            L.bump();
            size_t start = L.pos;
            while (ident_cont(L.cur())) L.bump();
            std::string text = L.src.substr(start - 1, L.pos - start + 1);
            out.push_back(L.make(Tok::Directive, b, std::move(text)));
            continue;
        }

        // --- @ (системные вызовы и атрибуты) ---
        if (c == '@') {
            L.bump();
            out.push_back(L.make(Tok::At, b, "@"));
            continue;
        }

        // --- операторы и пунктуация ---
        L.bump();
        char n = L.cur();
        switch (c) {
            case '+': out.push_back(L.make(Tok::Plus, b, "+")); continue;
            case '-':
                if (n == '>') { L.bump(); out.push_back(L.make(Tok::Arrow, b, "->")); }
                else out.push_back(L.make(Tok::Minus, b, "-"));
                continue;
            case '*': out.push_back(L.make(Tok::Star, b, "*")); continue;
            case '/': out.push_back(L.make(Tok::Slash, b, "/")); continue;
            case '%': out.push_back(L.make(Tok::Percent, b, "%")); continue;
            case '&':
                if (n == '&') { L.bump(); out.push_back(L.make(Tok::And, b, "&&")); }
                else out.push_back(L.make(Tok::Amp, b, "&"));
                continue;
            case '|':
                if (n == '|') { L.bump(); out.push_back(L.make(Tok::Or, b, "||")); }
                else out.push_back(L.make(Tok::Pipe, b, "|"));
                continue;
            case '^': out.push_back(L.make(Tok::Caret, b, "^")); continue;
            case '~': out.push_back(L.make(Tok::Tilde, b, "~")); continue;
            case '!':
                if (n == '=') { L.bump(); out.push_back(L.make(Tok::NotEq, b, "!=")); }
                else out.push_back(L.make(Tok::Bang, b, "!"));
                continue;
            case '=':
                if (n == '=') { L.bump(); out.push_back(L.make(Tok::Eq, b, "==")); }
                else if (n == '>') { L.bump(); out.push_back(L.make(Tok::FatArrow, b, "=>")); }
                else out.push_back(L.make(Tok::Assign, b, "="));
                continue;
            case '<':
                if (n == '=') { L.bump(); out.push_back(L.make(Tok::Le, b, "<=")); }
                else if (n == '<') { L.bump(); out.push_back(L.make(Tok::Shl, b, "<<")); }
                else out.push_back(L.make(Tok::Lt, b, "<"));
                continue;
            case '>':
                if (n == '=') { L.bump(); out.push_back(L.make(Tok::Ge, b, ">=")); }
                else if (n == '>') { L.bump(); out.push_back(L.make(Tok::Shr, b, ">>")); }
                else out.push_back(L.make(Tok::Gt, b, ">"));
                continue;
            case '.': out.push_back(L.make(Tok::Dot, b, ".")); continue;
            case ',': out.push_back(L.make(Tok::Comma, b, ",")); continue;
            case ';': out.push_back(L.make(Tok::Semi, b, ";")); continue;
            case ':': out.push_back(L.make(Tok::Colon, b, ":")); continue;
            case '?': out.push_back(L.make(Tok::Question, b, "?")); continue;
            case '(': out.push_back(L.make(Tok::LParen, b, "(")); continue;
            case ')': out.push_back(L.make(Tok::RParen, b, ")")); continue;
            case '[': out.push_back(L.make(Tok::LBracket, b, "[")); continue;
            case ']': out.push_back(L.make(Tok::RBracket, b, "]")); continue;
            case '{': out.push_back(L.make(Tok::LBrace, b, "{")); continue;
            case '}': out.push_back(L.make(Tok::RBrace, b, "}")); continue;
            default: {
                Span sp = L.span_of(b);
                std::string ch(1, c);
                diags.error("K0001", "неизвестный символ '" + ch + "'", sp)
                    .hint("в KINII используются: буквы, цифры, _ и операторы + - * / % = < > ! & | ^ ~ ?")
                    .emit();
                out.push_back(L.make(Tok::Unknown, b, ch));
                continue;
            }
        }
    }

    return out;
}

const char* tok_name(Tok t) {
    switch (t) {
        case Tok::Ident: return "Ident";
        case Tok::Number: return "Number";
        case Tok::String: return "String";
        case Tok::SqlTag: return "SqlTag";
        case Tok::Directive: return "Directive";
        case Tok::Bridge: return "Bridge";
        case Tok::At: return "At";
        case Tok::KwLet: return "let";
        case Tok::KwConst: return "const";
        case Tok::KwFn: return "fn";
        case Tok::KwStruct: return "struct";
        case Tok::KwEnum: return "enum";
        case Tok::KwNode: return "node";
        case Tok::KwIsolate: return "isolate";
        case Tok::KwInterface: return "interface";
        case Tok::KwPage: return "page";
        case Tok::KwAppEntry: return "app_entry";
        case Tok::KwSpawn: return "spawn";
        case Tok::KwImport: return "import";
        case Tok::KwFrom: return "from";
        case Tok::KwAs: return "as";
        case Tok::KwPub: return "pub";
        case Tok::KwPriv: return "priv";
        case Tok::KwAsync: return "async";
        case Tok::KwAwait: return "await";
        case Tok::KwEntry: return "entry";
        case Tok::KwHandler: return "handler";
        case Tok::KwIf: return "if";
        case Tok::KwElse: return "else";
        case Tok::KwWhile: return "while";
        case Tok::KwLoop: return "loop";
        case Tok::KwFor: return "for";
        case Tok::KwIn: return "in";
        case Tok::KwReturn: return "return";
        case Tok::KwMove: return "move";
        case Tok::KwDecide: return "decide";
        case Tok::KwWeight: return "weight";
        case Tok::KwBreak: return "break";
        case Tok::KwContinue: return "continue";
        case Tok::KwTrue: return "true";
        case Tok::KwFalse: return "false";
        case Tok::KwCell: return "cell";
        case Tok::KwType: return "type";
        case Tok::KwOperator: return "operator";
        case Tok::KwDefine: return "define";
        case Tok::KwTag: return "tag";
        case Tok::KwAsm: return "asm";
        case Tok::TyU8: return "u8";
        case Tok::TyU16: return "u16";
        case Tok::TyU32: return "u32";
        case Tok::TyU64: return "u64";
        case Tok::TyString: return "string";
        case Tok::TyBool: return "bool";
        case Tok::TyPtr: return "ptr";
        case Tok::TyVWord: return "v_word";
        case Tok::TyVAddr: return "v_addr";
        case Tok::TyList: return "list";
        case Tok::Plus: return "+";
        case Tok::Minus: return "-";
        case Tok::Star: return "*";
        case Tok::Slash: return "/";
        case Tok::Percent: return "%";
        case Tok::Amp: return "&";
        case Tok::Pipe: return "|";
        case Tok::Caret: return "^";
        case Tok::Tilde: return "~";
        case Tok::Bang: return "!";
        case Tok::Assign: return "=";
        case Tok::Eq: return "==";
        case Tok::NotEq: return "!=";
        case Tok::Lt: return "<";
        case Tok::Gt: return ">";
        case Tok::Le: return "<=";
        case Tok::Ge: return ">=";
        case Tok::And: return "&&";
        case Tok::Or: return "||";
        case Tok::Shl: return "<<";
        case Tok::Shr: return ">>";
        case Tok::Arrow: return "->";
        case Tok::FatArrow: return "=>";
        case Tok::Dot: return ".";
        case Tok::Comma: return ",";
        case Tok::Semi: return ";";
        case Tok::Colon: return ":";
        case Tok::Question: return "?";
        case Tok::LParen: return "(";
        case Tok::RParen: return ")";
        case Tok::LBracket: return "[";
        case Tok::RBracket: return "]";
        case Tok::LBrace: return "{";
        case Tok::RBrace: return "}";
        case Tok::Eof: return "EOF";
        case Tok::Unknown: return "UNKNOWN";
    }
    return "?";
}

} // namespace kinii
