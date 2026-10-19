// Форматтер KINII («prettier для нашего языка»).
//
// Принципы — честные и предсказуемые:
//   • работает по ПОТОКУ ТОКЕНОВ, а не по AST: комментарии живут, потому что
//     лексер в режиме keep_comments отдаёт их как Tok::Comment;
//   • отступ 4 пробела на уровень '{}', одна инструкция — одна строка;
//   • пробелы вокруг бинарных операторов, скобки прилипают к именам;
//   • ";", "{", "}", комментарий завершают строку;
//   • "} else {"/"} else if (...) {" остаются одной строкой;
//   • короткие inline-литералы (Точка { х: 5; у: 7 };) остаются в одну строку;
//   • между блоками сохраняется максимум ОДНА пустая строка;
//   • длинные строки не переносим (v1: не рвём за человека).

#include "kinii/lexer.h"

#include <cstdint>
#include <string>
#include <vector>

namespace kinii {

namespace {

bool is_word(Tok t) {
    switch (t) {
        case Tok::Ident: case Tok::Number: case Tok::String: case Tok::SqlTag:
            return true;
        default:
            return t >= Tok::KwLet && t <= Tok::TyList; // ключевые слова и типы
    }
}

bool is_word_end(Tok t) { // выражение закончилось → следующий '-' уже бинарный
    return is_word(t) || t == Tok::RParen || t == Tok::RBracket;
}

bool is_type(Tok t) { return t >= Tok::TyU8 && t <= Tok::TyList; }

bool no_space_before(Tok t) {
    switch (t) {
        case Tok::RParen: case Tok::RBracket: case Tok::Comma: case Tok::Semi:
        case Tok::Colon: case Tok::Dot:
            return true;
        default: return false;
    }
}

bool no_space_after(Tok t) {
    switch (t) {
        case Tok::LParen: case Tok::LBracket: case Tok::At:
        case Tok::Bang: case Tok::Tilde: case Tok::Dot:
            return true;
        default: return false;
    }
}

// '(' и '[' прилипают к именам и вызовоподобным ключевым словам:
// fn f(, xs[0], decide(cmd), weight("go"); а if/while/for — с пробелом.
bool glues_left(Tok prev, Tok t) {
    if (t == Tok::LParen) {
        switch (prev) {
            case Tok::Ident: case Tok::At:
            case Tok::KwDecide: case Tok::KwWeight:
                return true;
            default: return false;
        }
    }
    if (t == Tok::LBracket) {
        switch (prev) {
            case Tok::Ident: case Tok::RParen: case Tok::RBracket:
                return true;
            default: return false;
        }
    }
    return false;
}

bool is_unary_op(Tok t) {
    return t == Tok::Minus || t == Tok::Plus || t == Tok::Bang || t == Tok::Tilde;
}

// После этих токенов '{' — это начало короткого inline-литерала, а не блок.
bool inline_open_context(Tok prev) {
    switch (prev) {
        case Tok::Assign: case Tok::LParen: case Tok::Comma: case Tok::Colon:
        case Tok::FatArrow: case Tok::Question: case Tok::KwReturn: case Tok::KwSpawn:
            return true;
        default: return false;
    }
}

} // namespace

std::string format_source(FileId fid, const std::string& src) {
    DiagnosticEngine dg; // тихий: форматтер не судья; честные ошибки отдаёт check
    std::vector<Token> toks = lex_file(fid, src, dg, /*keep_comments=*/true);

    std::string out;
    std::string line;
    int depth = 0;
    int inline_lit = 0; // глубина внутри однострочного struct-литерала
    uint32_t prev_end_line = 0;
    Tok prev = Tok::Eof;
    Tok prev_prev = Tok::Eof;

    auto indent = [&]() { return std::string(size_t(depth > 0 ? depth : 0) * 4, ' '); };
    auto endl = [&]() {
        out += line;
        out += '\n';
        line.clear();
    };
    auto put = [&](const std::string& s) {
        if (line.empty()) line = indent();
        line += s;
    };
    auto put_sp = [&]() { // один пробел, если строка уже начата
        if (!line.empty() && line.back() != ' ') line += ' ';
    };
    auto with_sep = [&](bool want) { // премиум-пробел по правилам ниже; печать вместе
        return want;
    };
    (void)with_sep;

    auto match_brace = [&](size_t i) -> size_t {
        int d = 0;
        for (size_t j = i; j < toks.size(); ++j) {
            if (toks[j].type == Tok::LBrace) ++d;
            else if (toks[j].type == Tok::RBrace) {
                --d;
                if (d == 0) return j;
            }
        }
        return SIZE_MAX;
    };

    for (size_t i = 0; i < toks.size(); ++i) {
        const Token& t = toks[i];
        if (t.type == Tok::Eof) break;

        // Пустая строка между блоками: 2+ пустые строки в исходнике → одна.
        if (inline_lit == 0 && line.empty() && prev_end_line != 0 &&
            t.begin.line > prev_end_line + 1) {
            // не оставляем две пустые подряд (после endl уже есть '\n')
            if (out.size() < 2 || out.compare(out.size() - 2, 2, "\n\n") != 0) out += '\n';
        }

        if (inline_lit > 0) {
            // Внутри однострочного литерала всё живёт на одной строке.
            bool sp = !line.empty() && line.back() != ' ';
            if (no_space_before(t.type)) sp = false;
            else if (t.type == Tok::Colon) {
                // ':' после имени — метка поля «х: …» (плотный);
                // ':' после значения — тернарник «1 : 2» (свободный)
                sp = (prev != Tok::Ident);
            }
            else if (no_space_after(prev)) sp = false;
            else if (glues_left(prev, t.type)) sp = false;
            else if (is_unary_op(prev) && !is_word_end(prev_prev)) sp = false;
            else if ((t.type == Tok::Minus || t.type == Tok::Plus) && !is_word_end(prev))
                sp = false;
            if (sp) put_sp();
            put(t.text);
            prev_prev = prev;
            prev = t.type;
            prev_end_line = t.end.line;
            if (t.type == Tok::LBrace) { /* вложенный литерал — просто открываем */ ++inline_lit; continue; }
            if (t.type == Tok::RBrace) {
                --inline_lit;
                if (inline_lit == 0) {
                    // "… };" и "… )" держим рядом с '}' на одной строке:
                    // endl откладывается до следующего токена.
                    size_t j = i + 1;
                    while (j < toks.size() && toks[j].type == Tok::Comment) ++j;
                    Tok nxt = (j < toks.size()) ? toks[j].type : Tok::Eof;
                    if (nxt != Tok::Semi && nxt != Tok::Comma && nxt != Tok::RParen &&
                        nxt != Tok::RBracket)
                        endl();
                }
                // вложенный '}' (inline_lit > 0): строка общая, не рвём.
                continue;
            }
            continue; // ';' внутри литерала NOT endl: строки не рвём
        }

        switch (t.type) {
            case Tok::Comment: {
                // хвостовой комментарий прилипает к коду, отдельный — живёт своей строкой
                if (!line.empty()) line += "  ";
                put(t.text);
                endl();
                break;
            }

            case Tok::Directive: { // #if / #endif — всегда с новой строки
                if (!line.empty()) endl();
                put(t.text);
                endl();
                break;
            }

            case Tok::Semi:
                put(t.text);
                endl();
                break;

            case Tok::LBrace: {
                size_t m = match_brace(i);
                // литерал открывается после '=' / '(' / ',' прямо или через имя типа:
                //= Точка { ... | print(Точка { ... | weight 3 => { ...
                bool opening = inline_open_context(prev) || inline_open_context(prev_prev);
                bool inlineable = opening && m != SIZE_MAX &&
                                  toks[m].begin.line == t.begin.line;
                if (inlineable) {
                    // короткий литерал: держим в одну строку до парной '}'
                    put_sp();
                    put(t.text);
                    inline_lit = 1;
                    break;
                }
                put_sp();
                put(t.text);
                endl();
                depth++;
                break;
            }

            case Tok::RBrace: {
                if (!line.empty()) endl();
                depth--;
                put(t.text);
                // "} else" не флашим: else / else-if прилипнут к той же строке.
                size_t j = i + 1;
                while (j < toks.size() &&
                       (toks[j].type == Tok::Comment || toks[j].type == Tok::Eof))
                    ++j;
                if (j < toks.size() && toks[j].type == Tok::KwElse) break;
                endl();
                break;
            }

            default: {
                bool sp = !line.empty() && line.back() != ' ';
                if (no_space_before(t.type)) {
                    sp = false;
                } else if (t.type == Tok::Colon) {
                    // ':' после имени — метка «поле: тип» (плотный);
                    // ':' после значения — тернарник «1 : 2» (свободный)
                    if (prev == Tok::Ident) sp = false; else sp = true;
                }
                if (sp) {
                    if (no_space_after(prev)) sp = false;
                    else if (glues_left(prev, t.type)) sp = false;
                    else if (is_unary_op(prev) && !is_word_end(prev_prev)) sp = false;
                    else if ((t.type == Tok::Minus || t.type == Tok::Plus) && !is_word_end(prev))
                        sp = false; // унарный '+'/'-': приклей
                }
                if (sp) put_sp();
                put(t.text);
                break;
            }
        }

        prev_prev = prev;
        prev = t.type;
        prev_end_line = t.end.line;
    }

    if (!line.empty()) endl();

    // Никогда не возвращаем хуже, чем было.
    if (out.empty()) return src;
    return out;
}

} // namespace kinii
