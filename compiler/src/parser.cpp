// Парсер KINII — рекурсивный спуск с восстановлением после ошибок.
// Принцип диагностик: каждая ошибка отвечает на три вопроса —
// ЧТО не так, ГДЕ (фрагмент с кареткой) и КАК ПРАВИЛЬНО (пример).

#include "kinii/parser.h"

#include <algorithm>
#include <cstdlib>

namespace kinii {

namespace {

// Расстояние Левенштейна — для подсказок «возможно, вы имели в виду ...».
size_t lev(const std::string& a, const std::string& b) {
    const size_t n = a.size(), m = b.size();
    std::vector<size_t> prev(m + 1), cur(m + 1);
    for (size_t j = 0; j <= m; ++j) prev[j] = j;
    for (size_t i = 1; i <= n; ++i) {
        cur[0] = i;
        for (size_t j = 1; j <= m; ++j) {
            size_t cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost});
        }
        std::swap(prev, cur);
    }
    return prev[m];
}

const char* kCellMods[] = {"vocal", "raw"};

std::string describe(const Token& t) {
    return (t.type == Tok::Eof) ? std::string("конец файла") : "'" + t.text + "'";
}

class Parser {
public:
    Parser(FileId fid, const std::vector<Token>& toks, DiagnosticEngine& dg)
        : fid_(fid), toks_(toks), dg_(dg) {}

    StmtP parse_program() {
        auto prog = std::make_unique<Stmt>(Stmt::Block);
        if (!toks_.empty()) {
            prog->span = span_loc(toks_.front().begin, toks_.back().end);
        }
        while (!check(Tok::Eof)) {
            size_t before = i_;
            auto st = parse_top();
            if (st) prog->body.push_back(std::move(st));
            if (i_ == before) advance(); // страховка от зацикливания
        }
        // Незакрытые скобки: показываем, ГДЕ открыта и ЧЕГО не хватает в конце.
        for (const auto& loc : braces_) {
            Span open;
            open.file = fid_;
            open.begin = loc;
            open.end = loc;
            Span eof;
            eof.file = fid_;
            eof.begin = eof.end = peek().begin;
            dg_.error("K1008", "не закрыта фигурная скобка '{' — дошёл до конца файла", eof)
                .label(open, "скобка открыта здесь")
                .hint("каждой '{' соответствует своя '}'")
                .fix("как должно выглядеть:",
                     "app_entry {\n"
                     "    ...\n"
                     "}")   // конец блока
                .emit();
        }
        return prog;
    }

private:
    FileId fid_;
    const std::vector<Token>& toks_;
    DiagnosticEngine& dg_;
    size_t i_ = 0;
    std::vector<SourceLocation> braces_; // стек открытых '{'

    // ---- служебные ----

    const Token& peek(size_t d = 0) const {
        size_t idx = i_ + d;
        if (idx >= toks_.size()) idx = toks_.size() - 1;
        return toks_[idx];
    }
    const Token& advance() {
        const Token& t = toks_[i_];
        if (i_ + 1 < toks_.size()) ++i_;
        return t;
    }
    bool check(Tok t) const { return peek().type == t; }
    bool match(Tok t) {
        if (check(t)) {
            advance();
            return true;
        }
        return false;
    }
    SourceLocation prev_end() const {
        return (i_ > 0) ? toks_[i_ - 1].end : peek().begin;
    }
    Span span_of(const Token& t) const {
        Span s;
        s.file = fid_;
        s.begin = t.begin;
        s.end = t.end;
        return s;
    }
    Span span_loc(SourceLocation b, SourceLocation e) const {
        Span s;
        s.file = fid_;
        s.begin = b;
        s.end = e;
        return s;
    }
    Span span_join(const Span& a, const Span& b) const {
        Span s;
        s.file = fid_;
        s.begin = a.begin;
        s.end = b.end;
        return s;
    }

    void skip_line() {
        while (!check(Tok::Semi) && !check(Tok::Eof)) advance();
        match(Tok::Semi);
    }

    const Token& expect(Tok t, const std::string& what, const char* fix_snippet = nullptr) {
        if (check(t)) return advance();
        const Token& got = peek();
        Span sp = span_of(got);
        DiagnosticBuilder b = dg_.error("K1001", "ожидалось " + what + ", а найдено " + describe(got), sp);
        if (fix_snippet) b.fix("как должно выглядеть:", fix_snippet);
        b.emit();
        return got;
    }

    // ---- типы ----

    bool is_type_start(const Token& t) const {
        switch (t.type) {
            case Tok::TyU8: case Tok::TyU16: case Tok::TyU32: case Tok::TyU64:
            case Tok::TyString: case Tok::TyBool: case Tok::TyPtr:
            case Tok::TyVWord: case Tok::TyVAddr: case Tok::TyList: case Tok::TyMap:
            case Tok::KwCell:
            case Tok::Ident:
                return true;
            default:
                return false;
        }
    }

    TypeP parse_type() {
        auto ty = std::make_unique<TypeExpr>();
        ty->span.begin = peek().begin;
        const Token& t = peek();

        switch (t.type) {
            case Tok::TyU8: case Tok::TyU16: case Tok::TyU32: case Tok::TyU64:
            case Tok::TyString: case Tok::TyBool: case Tok::TyPtr:
            case Tok::TyVWord: case Tok::TyVAddr:
                advance();
                ty->kind = TypeExpr::Named;
                ty->name = t.text;
                break;

            case Tok::KwCell: {
                advance();
                ty->kind = TypeExpr::Cell;
                ty->name = "cell";
                if (match(Tok::Colon)) {
                    const Token& m = expect(Tok::Ident, "модификатор ячейки (vocal или raw)");
                    ty->name = m.text; // для Cell в name хранится модификатор
                    if (m.text != "vocal" && m.text != "raw") {
                        std::string best = "vocal";
                        size_t bd = SIZE_MAX;
                        for (const char* cm : kCellMods) {
                            size_t d = lev(m.text, cm);
                            if (d < bd) { bd = d; best = cm; }
                        }
                        auto b = dg_.error("K1002",
                                           "неизвестный модификатор ячейки '" + m.text + "'",
                                           span_of(m));
                        b.hint("существуют только: vocal (обычная ячейка) и raw (прямой указатель)");
                        if (bd <= 2) b.suggest("cell:" + best);
                        b.fix("как должно выглядеть:", "let cell:raw p = 0x1000;");
                        b.emit();
                    }
                }
                break;
            }

            case Tok::TyList: {
                advance();
                ty->kind = TypeExpr::List;
                expect(Tok::Lt, "'<' в list<T>", "let list<string> items = [\"a\"];");
                ty->elem = parse_type();
                expect(Tok::Gt, "'>' закрывающий list<T>");
                break;
            }

            case Tok::TyMap: {
                advance();
                ty->kind = TypeExpr::Map;
                expect(Tok::Lt, "'<' в map<K, V>",
                       "let map<string, u32> цены = [\"хлеб\": 50];");
                ty->elem = parse_type();  // ключ
                expect(Tok::Comma, "',' между ключом и значением в map<K, V>",
                       "let map<string, u32> цены = [\"хлеб\": 50];");
                ty->val = parse_type();   // значение
                expect(Tok::Gt, "'>' закрывающий map<K, V>");
                break;
            }

            case Tok::Ident:
                advance();
                ty->kind = TypeExpr::Named;
                ty->name = t.text;
                break;

            default: {
                dg_.error("K1003", "ожидался тип, а найдено " + describe(t), span_of(t))
                    .hint("примитивы: u8 u16 u32 u64 string bool ptr v_word v_addr")
                    .hint("также: cell, cell:raw, list<T> или имя структуры")
                    .fix("как должно выглядеть:", "let u32 count = 0;")
                    .emit();
                ty->kind = TypeExpr::Named;
                ty->name = "<ошибка>";
                break;
            }
        }

        // Массив: u32[1024]
        if (check(Tok::LBracket)) {
            advance();
            ty->kind = TypeExpr::Array;
            const Token& n = expect(Tok::Number, "длина массива");
            ty->array_len = (uint32_t)strtoul(n.text.c_str(), nullptr, 10);
            expect(Tok::RBracket, "']' после длины массива");
        }

        ty->span.end = prev_end();
        return ty;
    }

    // ---- объявления ----

    StmtP parse_let(bool is_const) {
        const Token& kw = advance(); // let / const
        auto st = std::make_unique<Stmt>(is_const ? Stmt::Const : Stmt::Let);
        st->span.begin = kw.begin;

        // Формы: let x = 5; | let u32 x = 5; | let x: u32 = 5; | let Point p = ...;
        const Token& first = peek();
        bool type_first = is_type_start(first) && first.type != Tok::Ident;
        if (first.type == Tok::Ident && peek(1).type == Tok::Ident) type_first = true;

        if (type_first) st->type = parse_type();

        const Token& nm = peek();
        if (nm.type == Tok::Ident) {
            advance();
        } else {
            // Одна ясная ошибка на строку: имя не прочиталось (например, кириллица
            // в имени). Дальше — перепрыг до конца строки, без каскада «ожидалось;»
            expect(Tok::Ident, "имя переменной");
            skip_line();
            return st;
        }
        st->name = nm.text;
        st->name_span = span_of(nm);

        if (!type_first && match(Tok::Colon)) st->type = parse_type();

        if (match(Tok::Assign)) {
            st->init = parse_expr();
        } else if (is_const) {
            dg_.error("K1007", "константа '" + st->name + "' объявлена без значения", span_of(peek()))
                .hint("const — значение, которое никогда не меняется; ему нужно присвоение")
                .fix("как должно выглядеть:", "const MAX_RETRY = 3;")
                .emit();
        }

        expect(Tok::Semi, "';' в конце объявления", "let u32 count = 50;");
        st->span = span_loc(kw.begin, prev_end());
        return st;
    }

    StmtP parse_fn() {
        auto st = std::make_unique<Stmt>(Stmt::FnDecl);
        st->span.begin = peek().begin;

        for (;;) {
            if (match(Tok::KwPub)) st->is_pub = true;
            else if (match(Tok::KwPriv)) st->is_priv = true;
            else if (match(Tok::KwAsync)) st->is_async = true;
            else if (match(Tok::KwEntry)) st->is_entry = true;
            else if (match(Tok::KwHandler)) st->is_handler = true;
            else break;
        }

        expect(Tok::KwFn, "'fn' (перед ним могут быть только pub/priv/async/entry/handler)");
        const Token& nm = expect(Tok::Ident, "имя функции");
        st->name = nm.text;
        st->name_span = span_of(nm);
        expect(Tok::LParen, "'(' после имени функции",
               "fn add(a: u32, b: u32) -> u32 { return a + b; }");

        if (!check(Tok::RParen)) {
            do {
                const Token& pn = expect(Tok::Ident, "имя параметра");
                expect(Tok::Colon, "':' после имени параметра");
                Param p;
                p.name = pn.text;
                p.span = span_of(pn);
                p.type = parse_type();
                st->params.push_back(std::move(p));
            } while (match(Tok::Comma));
        }
        expect(Tok::RParen, "')' после параметров");

        if (match(Tok::Arrow)) st->ret = parse_type();

        st->block = parse_block();
        st->span = span_loc(st->span.begin, prev_end());
        return st;
    }

    StmtP parse_struct() {
        const Token& kw = advance(); // struct
        auto st = std::make_unique<Stmt>(Stmt::StructDecl);
        st->span.begin = kw.begin;
        const Token& snm = expect(Tok::Ident, "имя структуры");
        st->name = snm.text;
        st->name_span = span_of(snm);
        expect(Tok::LBrace, "'{' после имени структуры", "struct Point { x: u32; }");
        braces_.push_back(peek().begin);

        while (!check(Tok::RBrace) && !check(Tok::Eof)) {
            size_t before = i_;
            const Token& fn0 = expect(Tok::Ident, "имя поля");
            expect(Tok::Colon, "':' после имени поля", "struct Point { x: u32; }");
            FieldDecl f;
            f.name = fn0.text;
            f.span = span_of(fn0);
            f.type = parse_type();
            expect(Tok::Semi, "';' после поля");
            st->fields.push_back(std::move(f));
            if (i_ == before) advance();
        }
        if (match(Tok::RBrace)) braces_.pop_back();

        st->span = span_loc(st->span.begin, prev_end());
        return st;
    }

    StmtP parse_enum() {
        const Token& kw = advance(); // enum
        auto st = std::make_unique<Stmt>(Stmt::EnumDecl);
        st->span.begin = kw.begin;
        const Token& snm = expect(Tok::Ident, "имя перечисления");
        st->name = snm.text;
        st->name_span = span_of(snm);
        expect(Tok::LBrace, "'{' после имени перечисления", "enum DataType { U8, U16, U32 }");
        braces_.push_back(peek().begin);

        while (!check(Tok::RBrace) && !check(Tok::Eof)) {
            size_t before = i_;
            st->enum_items.push_back(expect(Tok::Ident, "имя варианта").text);
            match(Tok::Comma);
            if (i_ == before) advance();
        }
        if (match(Tok::RBrace)) braces_.pop_back();

        st->span = span_loc(st->span.begin, prev_end());
        return st;
    }

    // ---- инструкции ----

    StmtP parse_block() {
        const Token& lb = peek();
        expect(Tok::LBrace, "'{'");
        braces_.push_back(lb.begin);

        auto st = std::make_unique<Stmt>(Stmt::Block);
        st->span.begin = lb.begin;
        while (!check(Tok::RBrace) && !check(Tok::Eof)) {
            size_t before = i_;
            auto s = parse_stmt();
            if (s) st->body.push_back(std::move(s));
            if (i_ == before) advance();
        }
        if (match(Tok::RBrace)) braces_.pop_back();

        st->span = span_loc(st->span.begin, prev_end());
        return st;
    }

    StmtP parse_if() {
        const Token& kw = advance(); // if
        auto st = std::make_unique<Stmt>(Stmt::If);
        st->span.begin = kw.begin;

        expect(Tok::LParen, "'(' после 'if'", "if (условие) { ... }");
        st->kids.push_back(parse_expr());
        // Если скобки забыли целиком — после условия идёт '{', второй ошибки не нужно.
        if (!match(Tok::RParen) && !check(Tok::LBrace))
            expect(Tok::RParen, "')' после условия if");

        auto then = parse_block();
        if (then) st->body.insert(st->body.end(),
                                  std::make_move_iterator(then->body.begin()),
                                  std::make_move_iterator(then->body.end()));

        if (match(Tok::KwElse)) {
            if (check(Tok::KwIf)) {
                st->else_body.push_back(parse_if());
            } else {
                auto eb = parse_block();
                if (eb) st->else_body.insert(st->else_body.end(),
                                             std::make_move_iterator(eb->body.begin()),
                                             std::make_move_iterator(eb->body.end()));
            }
        }
        st->span = span_loc(st->span.begin, prev_end());
        return st;
    }

    StmtP parse_while() {
        const Token& kw = advance();
        auto st = std::make_unique<Stmt>(Stmt::While);
        st->span.begin = kw.begin;
        expect(Tok::LParen, "'(' после 'while'", "while (условие) { ... }");
        st->kids.push_back(parse_expr());
        if (!match(Tok::RParen) && !check(Tok::LBrace))
            expect(Tok::RParen, "')' после условия while");
        st->body.push_back(parse_block());
        st->span = span_loc(st->span.begin, prev_end());
        return st;
    }

    StmtP parse_for() {
        const Token& kw = advance();
        auto st = std::make_unique<Stmt>(Stmt::ForIn);
        st->span.begin = kw.begin;
        st->name = expect(Tok::Ident, "имя переменной цикла").text;
        expect(Tok::KwIn, "'in' в цикле for", "for item in items { ... }");
        st->kids.push_back(parse_expr());
        st->body.push_back(parse_block());
        st->span = span_loc(st->span.begin, prev_end());
        return st;
    }

    StmtP parse_decide() {
        const Token& kw = advance(); // decide
        auto st = std::make_unique<Stmt>(Stmt::Decide);
        st->span.begin = kw.begin;

        expect(Tok::LParen, "'(' после 'decide'", "decide (x) { weight(1) => { ... } else => { ... } }");
        st->kids.push_back(parse_expr());
        if (!match(Tok::RParen) && !check(Tok::LBrace))
            expect(Tok::RParen, "')' после значения decide");
        expect(Tok::LBrace, "'{' после decide (...)");
        braces_.push_back(peek().begin);

        while (check(Tok::KwWeight) || check(Tok::KwElse)) {
            Stmt::Case c;
            c.span.begin = peek().begin;
            if (match(Tok::KwWeight)) {
                expect(Tok::LParen, "'(' после 'weight'", "weight(значение) => { ... }");
                c.value = parse_expr();
                expect(Tok::RParen, "')' после значения weight");
            } else {
                advance(); // else
            }
            expect(Tok::FatArrow, "'=>' после значения weight", "weight(значение) => { ... }");
            auto blk = parse_block();
            if (blk) c.body = std::move(blk->body);
            c.span = span_loc(c.span.begin, prev_end());
            st->cases.push_back(std::move(c));
        }

        if (match(Tok::RBrace)) braces_.pop_back();

        st->span = span_loc(st->span.begin, prev_end());
        return st;
    }

    StmtP parse_expr_stmt() {
        auto st = std::make_unique<Stmt>(Stmt::ExprStmt);
        st->span.begin = peek().begin;
        ExprP e = parse_expr();
        if (e) {
            st->span = span_loc(st->span.begin, e->span.end);
            st->kids.push_back(std::move(e));
        } else {
            // Выражения не было вовсе — «пропущена ';'» не в тему:
            // причину уже назвал K1012/K0001, не множим диагностики.
            return st;
        }
        if (!match(Tok::Semi)) {
            // Каретка — в конце выражения, где не хватает ';'.
            Span sp;
            sp.file = fid_;
            sp.begin = sp.end = st->span.end;
            dg_.error("K1004", "пропущена ';' после выражения", sp)
                .hint("каждая инструкция заканчивается точкой с запятой")
                .fix("как должно выглядеть:", "count = count + 1;")
                .emit();
        }
        return st;
    }

    StmtP parse_stmt() {
        switch (peek().type) {
            case Tok::KwLet: return parse_let(false);
            case Tok::KwConst: return parse_let(true);
            case Tok::KwIf: return parse_if();
            case Tok::KwWhile: return parse_while();
            case Tok::KwLoop: {
                const Token& kw = advance();
                auto st = std::make_unique<Stmt>(Stmt::Loop);
                st->span.begin = kw.begin;
                st->body.push_back(parse_block());
                st->span = span_loc(st->span.begin, prev_end());
                return st;
            }
            case Tok::KwFor: return parse_for();
            case Tok::KwDecide: return parse_decide();
            case Tok::KwReturn: {
                const Token& kw = advance();
                auto st = std::make_unique<Stmt>(Stmt::Return);
                st->span.begin = kw.begin;
                if (!check(Tok::Semi) && !check(Tok::RBrace)) st->kids.push_back(parse_expr());
                expect(Tok::Semi, "';' после return");
                st->span = span_loc(st->span.begin, prev_end());
                return st;
            }
            case Tok::KwBreak:
            case Tok::KwContinue: {
                const Token& kw = advance();
                auto st = std::make_unique<Stmt>(
                    kw.type == Tok::KwBreak ? Stmt::Break : Stmt::Continue);
                st->span.begin = kw.begin;
                expect(Tok::Semi, "';' после инструкции");
                st->span = span_loc(st->span.begin, prev_end());
                return st;
            }
            case Tok::KwFn: return parse_fn();
            case Tok::LBrace: return parse_block();
            case Tok::At: {
                Span sp = span_of(peek());
                dg_.error("K1010", "'@' здесь не нужен — он ставится перед системным вызовом внутри выражения", sp)
                    .hint("пример: let arch = @sys.bitness();")
                    .emit();
                skip_line();
                return nullptr;
            }
            default: return parse_expr_stmt();
        }
    }

    // ---- верхний уровень ----

    StmtP parse_top() {
        switch (peek().type) {
            case Tok::KwImport: {
                const Token& kw = advance();
                auto st = std::make_unique<Stmt>(Stmt::Import);
                st->span.begin = kw.begin;
                const Token& p = expect(Tok::String, "путь файла в кавычках", "import \"memory.ctos\";");
                st->name = p.value;
                expect(Tok::Semi, "';' после import");
                st->span = span_loc(kw.begin, prev_end());
                return st;
            }
            case Tok::KwStruct: return parse_struct();
            case Tok::KwEnum: return parse_enum();
            case Tok::KwNode:
            case Tok::KwIsolate: {
                bool node = peek().type == Tok::KwNode;
                const Token& kw = advance();
                auto st = std::make_unique<Stmt>(node ? Stmt::NodeDecl : Stmt::IsolateDecl);
                st->span.begin = kw.begin;
                const Token& inm = expect(Tok::Ident, node ? "имя узла" : "имя изолята");
                st->name = inm.text;
                st->name_span = span_of(inm);
                st->block = parse_block();
                st->span = span_loc(kw.begin, prev_end());
                return st;
            }
            case Tok::KwAppEntry: {
                const Token& kw = advance();
                auto st = std::make_unique<Stmt>(Stmt::AppEntry);
                st->span.begin = kw.begin;
                st->block = parse_block();
                st->span = span_loc(kw.begin, prev_end());
                return st;
            }
            case Tok::KwFn:
            case Tok::KwPub: case Tok::KwPriv:
            case Tok::KwAsync: case Tok::KwEntry: case Tok::KwHandler:
                return parse_fn();
            case Tok::KwLet: return parse_let(false);
            case Tok::KwConst: return parse_let(true);
            case Tok::Bridge: {
                dg_.error("K1011", "оператор '&-' убран из языка v3", span_of(peek()))
                    .hint("импорт теперь обычный: import \"файл.ctos\";")
                    .hint("вызовы системных функций — обычные: sys.hw.init();")
                    .fix("как должно выглядеть:", "import \"kernel.ctos\";")
                    .emit();
                skip_line();
                return nullptr;
            }
            case Tok::RBrace: {
                dg_.error("K1009", "лишняя '}' — перед ней нет открытой '{'", span_of(peek()))
                    .hint("проверьте, не пропущена ли открывающая скобка выше")
                    .emit();
                advance();
                return nullptr;
            }
            default: return parse_expr_stmt();
        }
    }

    // ---- выражения ----

    ExprP mkbin(Tok op, ExprP l, ExprP r, const Span& sp) {
        auto e = std::make_unique<Expr>(Expr::Binary);
        e->text = tok_name(op);
        e->span = sp;
        e->kids.push_back(std::move(l));
        e->kids.push_back(std::move(r));
        return e;
    }

    ExprP parse_expr() { return parse_assign(); }
    ExprP parse_assign() {
        ExprP lhs = parse_pipeline();
        if (!lhs) return nullptr;
        if (check(Tok::Assign)) {
            advance();
            if (lhs->kind != Expr::Ident && lhs->kind != Expr::Member &&
                lhs->kind != Expr::Index) {
                dg_.error("K1005", "слева от '=' должна быть переменная, поле или элемент",
                          lhs->span)
                    .hint("примеры: count = 5;   point.x = 1;   map[\"k\"] = 2;")
                    .emit();
            }
            ExprP rhs = parse_assign();
            if (!rhs) return nullptr;
            auto e = std::make_unique<Expr>(Expr::Assign);
            e->text = "=";
            e->span = span_join(lhs->span, rhs->span);
            e->kids.push_back(std::move(lhs));
            e->kids.push_back(std::move(rhs));
            return e;
        }
        return lhs;
    }

    // Пайплайн: «xs |> этап(x)» — самый низкий приоритет (ниже '=' корректно
    // вставляет правую часть присваивания). Трансформируем честным вызовом:
    // a |> f(b)  ==  f(a, b);  a |> f  ==  f(a). Проверки дерева семантики и
    // бэкенда работают как для обычного вызова — код не ветвится.
    ExprP pipeline(const Token& op, ExprP l, ExprP r) {
        if (!l || !r) return nullptr;
        if (r->kind == Expr::Call && !r->kids.empty()) {
            // span до перемещений
            const Span sp = span_join(l->span, r->span);
            r->kids.insert(r->kids.begin() + 1, std::move(l));
            r->span = sp;
            return r;
        }
        if (r->kind == Expr::Ident) {
            auto e = std::make_unique<Expr>(Expr::Call);
            e->span = span_join(l->span, r->span);
            e->kids.push_back(std::move(r));
            e->kids.push_back(std::move(l));
            return e;
        }
        dg_.error("K1006", "справа от '|>' должна быть функция или вызов", span_of(op))
            .hint("«|>» подставляет левое значение первым аргументом: "
                  "xs |> отсортировать() | xs |> прибавить(2); голая переменная "
                  "не пойдёт — нужен вызов")
            .emit();
        return nullptr;
    }

    ExprP parse_pipeline() {
        ExprP l = parse_ternary();
        while (l && check(Tok::Pipeline)) {
            const Token& op = advance();
            ExprP r = parse_ternary();
            l = pipeline(op, std::move(l), std::move(r));
        }
        return l;
    }

    ExprP parse_ternary() {
        ExprP c = parse_or();
        if (!c || !check(Tok::Question)) return c;
        advance();
        ExprP a = parse_ternary();
        expect(Tok::Colon, "':' в тернарном операторе", "x = cond ? a : b;");
        ExprP b = parse_ternary();
        if (!a || !b) return nullptr;
        auto e = std::make_unique<Expr>(Expr::Ternary);
        e->span = span_join(c->span, b->span);
        e->kids.push_back(std::move(c));
        e->kids.push_back(std::move(a));
        e->kids.push_back(std::move(b));
        return e;
    }

    ExprP parse_or() {
        ExprP l = parse_and();
        while (l && check(Tok::Or)) {
            const Token& op = advance();
            ExprP r = parse_and();
            if (!r) return nullptr;
            l = mkbin(op.type, std::move(l), std::move(r), span_join(l->span, r->span));
        }
        return l;
    }

    ExprP parse_and() {
        ExprP l = parse_bitor();
        while (l && check(Tok::And)) {
            const Token& op = advance();
            ExprP r = parse_bitor();
            if (!r) return nullptr;
            l = mkbin(op.type, std::move(l), std::move(r), span_join(l->span, r->span));
        }
        return l;
    }

    ExprP parse_bitor() {
        ExprP l = parse_bitxor();
        while (l && check(Tok::Pipe)) {
            const Token& op = advance();
            ExprP r = parse_bitxor();
            if (!r) return nullptr;
            l = mkbin(op.type, std::move(l), std::move(r), span_join(l->span, r->span));
        }
        return l;
    }

    ExprP parse_bitxor() {
        ExprP l = parse_bitand();
        while (l && check(Tok::Caret)) {
            const Token& op = advance();
            ExprP r = parse_bitand();
            if (!r) return nullptr;
            l = mkbin(op.type, std::move(l), std::move(r), span_join(l->span, r->span));
        }
        return l;
    }

    ExprP parse_bitand() {
        ExprP l = parse_equality();
        while (l && check(Tok::Amp)) {
            const Token& op = advance();
            ExprP r = parse_equality();
            if (!r) return nullptr;
            l = mkbin(op.type, std::move(l), std::move(r), span_join(l->span, r->span));
        }
        return l;
    }

    ExprP parse_equality() {
        ExprP l = parse_comparison();
        while (l && (check(Tok::Eq) || check(Tok::NotEq))) {
            const Token& op = advance();
            ExprP r = parse_comparison();
            if (!r) return nullptr;
            l = mkbin(op.type, std::move(l), std::move(r), span_join(l->span, r->span));
        }
        return l;
    }

    ExprP parse_comparison() {
        ExprP l = parse_shift();
        while (l && (check(Tok::Lt) || check(Tok::Gt) || check(Tok::Le) || check(Tok::Ge))) {
            const Token& op = advance();
            ExprP r = parse_shift();
            if (!r) return nullptr;
            l = mkbin(op.type, std::move(l), std::move(r), span_join(l->span, r->span));
        }
        return l;
    }

    ExprP parse_shift() {
        ExprP l = parse_term();
        while (l && (check(Tok::Shl) || check(Tok::Shr))) {
            const Token& op = advance();
            ExprP r = parse_term();
            if (!r) return nullptr;
            l = mkbin(op.type, std::move(l), std::move(r), span_join(l->span, r->span));
        }
        return l;
    }

    ExprP parse_term() {
        ExprP l = parse_factor();
        while (l && (check(Tok::Plus) || check(Tok::Minus))) {
            const Token& op = advance();
            ExprP r = parse_factor();
            if (!r) return nullptr;
            l = mkbin(op.type, std::move(l), std::move(r), span_join(l->span, r->span));
        }
        return l;
    }

    ExprP parse_factor() {
        ExprP l = parse_unary();
        while (l && (check(Tok::Star) || check(Tok::Slash) || check(Tok::Percent))) {
            const Token& op = advance();
            ExprP r = parse_unary();
            if (!r) return nullptr;
            l = mkbin(op.type, std::move(l), std::move(r), span_join(l->span, r->span));
        }
        return l;
    }

    ExprP parse_unary() {
        if (check(Tok::Bang) || check(Tok::Minus) || check(Tok::Amp)) {
            const Token& op = advance();
            ExprP x = parse_unary();
            if (!x) return nullptr;
            auto e = std::make_unique<Expr>(Expr::Unary);
            e->text = tok_name(op.type);
            e->span = span_join(span_of(op), x->span);
            e->kids.push_back(std::move(x));
            return e;
        }
        return parse_postfix();
    }

    ExprP parse_postfix() {
        ExprP e = parse_primary();
        if (!e) return nullptr;
        for (;;) {
            if (check(Tok::LParen)) {
                advance();
                auto call = std::make_unique<Expr>(Expr::Call);
                call->span.begin = e->span.begin;
                call->kids.push_back(std::move(e));
                if (!check(Tok::RParen)) {
                    do {
                        ExprP a = parse_expr();
                        if (!a) break;
                        call->kids.push_back(std::move(a));
                    } while (match(Tok::Comma));
                }
                expect(Tok::RParen, "')' после аргументов вызова");
                call->span = span_loc(call->span.begin, prev_end());
                e = std::move(call);
            } else if (check(Tok::Dot)) {
                advance();
                const Token& f = expect(Tok::Ident, "имя поля после '.'");
                auto m = std::make_unique<Expr>(Expr::Member);
                m->field = f.text;
                m->span = span_join(e->span, span_of(f));
                m->kids.push_back(std::move(e));
                e = std::move(m);
            } else if (check(Tok::LBracket)) {
                advance();
                auto ix = std::make_unique<Expr>(Expr::Index);
                ix->span.begin = e->span.begin;
                ix->kids.push_back(std::move(e));
                ExprP idx = parse_expr();
                if (idx) ix->kids.push_back(std::move(idx));
                expect(Tok::RBracket, "']' после индекса");
                ix->span = span_loc(ix->span.begin, prev_end());
                e = std::move(ix);
            } else {
                break;
            }
        }
        return e;
    }

    ExprP parse_primary() {
        const Token& t = peek();
        switch (t.type) {
            case Tok::Number: {
                advance();
                auto e = std::make_unique<Expr>(Expr::Num);
                e->text = t.text;
                e->span = span_of(t);
                return e;
            }
            case Tok::String: {
                advance();
                auto e = std::make_unique<Expr>(Expr::Str);
                e->text = t.value;
                e->span = span_of(t);
                return e;
            }
            case Tok::KwTrue:
            case Tok::KwFalse: {
                advance();
                auto e = std::make_unique<Expr>(Expr::Bool);
                e->text = t.text;
                e->span = span_of(t);
                return e;
            }
            case Tok::Ident: {
                advance();
                auto e = std::make_unique<Expr>(Expr::Ident);
                e->text = t.text;
                e->span = span_of(t);
                // Структура: Point { x: 1, y: 2 } — но не блок `for x in xs {`
                if (check(Tok::LBrace)) {
                    bool looks_like_lit =
                        (peek(1).type == Tok::RBrace) ||
                        (peek(1).type == Tok::Ident && peek(2).type == Tok::Colon);
                    if (looks_like_lit) {
                        advance();
                        auto s = std::make_unique<Expr>(Expr::StructLit);
                        s->field = t.text;
                        s->span.begin = t.begin;
                        while (!check(Tok::RBrace) && !check(Tok::Eof)) {
                            size_t before = i_;
                            const Token& fn0 = expect(Tok::Ident, "имя поля структуры");
                            expect(Tok::Colon, "':' после имени поля", "Point { x: 1, y: 2 }");
                            ExprP v = parse_expr();
                            if (v) s->kids.push_back(std::move(v));
                            s->fields.push_back(fn0.text);
                            if (!match(Tok::Comma)) break;
                            if (i_ == before) advance();
                        }
                        expect(Tok::RBrace, "'}' после полей структуры");
                        s->span = span_loc(s->span.begin, prev_end());
                        return s;
                    }
                }
                return e;
            }
            case Tok::LParen: {
                advance();
                ExprP inner = parse_expr();
                if (!inner) return nullptr;
                expect(Tok::RParen, "')' после выражения");
                Span s2;
                s2.file = fid_;
                s2.begin = t.begin;
                s2.end = prev_end();
                inner->span = s2;
                return inner;
            }
            case Tok::LBracket: {
                advance();
                auto e = std::make_unique<Expr>(Expr::ListLit);
                e->span.begin = t.begin;
                if (!check(Tok::RBracket)) {
                    ExprP first = parse_expr();
                    if (!first) return nullptr;
                    if (match(Tok::KwFor)) {
                        // comprehension: [x for x in xs if cond]
                        e->kind = Expr::Comprehension;
                        e->text = expect(Tok::Ident, "имя переменной в comprehension").text;
                        expect(Tok::KwIn, "'in' в comprehension", "[i for i in items if i != \"x\"]");
                        ExprP src = parse_expr();
                        if (!src) return nullptr;
                        e->kids.push_back(std::move(first));
                        e->kids.push_back(std::move(src));
                        if (match(Tok::KwIf)) {
                            ExprP cond = parse_expr();
                            if (!cond) return nullptr;
                            e->kids.push_back(std::move(cond));
                        }
                    } else if (match(Tok::Colon)) {
                        // словарь: ["хлеб": 50, "молоко": 90] — пары ключ: значение
                        e->kind = Expr::MapLit;
                        e->kids.push_back(std::move(first));    // ключ
                        ExprP v = parse_expr();
                        if (!v) return nullptr;
                        e->kids.push_back(std::move(v));        // значение
                        while (match(Tok::Comma)) {
                            if (check(Tok::RBracket)) break;    // запятая в конце — можно
                            ExprP k2 = parse_expr();
                            if (!k2) break;
                            expect(Tok::Colon, "':' между ключом и значением",
                                   "[\"хлеб\": 50, \"молоко\": 90]");
                            e->kids.push_back(std::move(k2));
                            ExprP v2 = parse_expr();
                            if (!v2) break;
                            e->kids.push_back(std::move(v2));
                        }
                    } else {
                        e->kids.push_back(std::move(first));
                        while (match(Tok::Comma)) {
                            if (check(Tok::RBracket)) break; // запятая в конце — можно
                            ExprP v = parse_expr();
                            if (!v) break;
                            e->kids.push_back(std::move(v));
                        }
                    }
                }
                expect(Tok::RBracket, "']' после списка");
                e->span = span_loc(e->span.begin, prev_end());
                return e;
            }
            case Tok::At: {
                advance();
                const Token& nm = expect(Tok::Ident, "имя системного модуля после '@'",
                                         "let arch = @sys.bitness();");
                auto e = std::make_unique<Expr>(Expr::Ident);
                e->text = "@" + nm.text;
                e->span = span_join(span_of(t), span_of(nm));
                return e;
            }
            case Tok::SqlTag: {
                dg_.warning("K1013",
                            "встроенный SQL '<?sql ?>' пока поддерживается только в .web (этап 5)",
                            span_of(t))
                    .emit();
                advance();
                auto e = std::make_unique<Expr>(Expr::Str);
                e->text = t.value;
                e->span = span_of(t);
                return e;
            }
            default: {
                // Токен Unknown — мусор, который уже честно пометил лексер (K0001).
                // Парсер молча перепрыгивает: без этого на каждую букву вне латыни
                // сыпался каскад «ожидалось выражение», и Problems превращался в
                // шум в десять ошибок на одну настоящую.
                if (t.type == Tok::Unknown) {
                    advance();
                    while (check(Tok::Unknown) && !check(Tok::Eof)) advance();
                    return parse_primary();
                }
                dg_.error("K1012", "ожидалось выражение (значение), а найдено " + describe(t),
                          span_of(t))
                    .hint("выражение может начинаться с: числа, строки, имени, (, [ или @")
                    .emit();
                return nullptr;
            }
        }
    }
};

} // namespace

StmtP parse_file(FileId fid, const std::vector<Token>& toks, DiagnosticEngine& diags) {
    Parser p(fid, toks, diags);
    return p.parse_program();
}

} // namespace kinii
