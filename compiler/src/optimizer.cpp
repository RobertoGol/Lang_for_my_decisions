// Оптимизатор: безопасные преобразования AST без изменения смысла программы.
// Всё зациклено до неподвижной точки: сворачиваем константы -> появляются новые
// константные условия -> уходят ветки -> уходит мёртвый код -> снова свёртка...

#include "kinii/optimizer.h"
#include "kinii/ast.h"

#include <cstdlib>

namespace kinii {

namespace {

// ---------- счётчик узлов (метрика «сколько места») ----------

size_t count_expr(const Expr* e) {
    if (!e) return 0;
    size_t n = 1;
    for (const auto& k : e->kids) n += count_expr(k.get());
    return n;
}

size_t count_nodes_impl(const Stmt* s) {
    if (!s) return 0;
    size_t n = 1;
    n += s->init ? count_expr(s->init.get()) : 0;
    for (const auto& k : s->kids) n += count_expr(k.get());
    for (const auto& b : s->body) n += count_nodes(b.get());
    for (const auto& b : s->else_body) n += count_nodes(b.get());
    if (s->block) n += count_nodes(s->block.get());
    for (const auto& c : s->cases) {
        n += c.value ? count_expr(c.value.get()) : 0;
        for (const auto& b : c.body) n += count_nodes(b.get());
    }
    return n;
}

// ---------- печать выражения для отчёта ----------

std::string show(const Expr* e) {
    if (!e) return "<...>";
    switch (e->kind) {
        case Expr::Num:
        case Expr::Bool:
        case Expr::Ident:
            return e->text;
        case Expr::Str:
            return "\"" + e->text + "\"";
        case Expr::Binary:
            return "(" + show(e->kids[0].get()) + " " + e->text + " " +
                   show(e->kids[1].get()) + ")";
        case Expr::Unary:
            return e->text + " " + show(e->kids[0].get());
        case Expr::Ternary:
            return show(e->kids[0].get()) + " ? " + show(e->kids[1].get()) +
                   " : " + show(e->kids[2].get());
        case Expr::Call: {
            std::string r = show(e->kids[0].get()) + "(";
            for (size_t i = 1; i < e->kids.size(); ++i) {
                if (i > 1) r += ", ";
                r += show(e->kids[i].get());
            }
            return r + ")";
        }
        case Expr::Member:
            return show(e->kids[0].get()) + "." + e->field;
        case Expr::Index:
            return show(e->kids[0].get()) + "[" +
                   (e->kids.size() > 1 ? show(e->kids[1].get()) : "") + "]";
        default:
            return "<выражение>";
    }
}

// "50", "0x1F", "-8" -> число
bool parse_int(const std::string& s, long long& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    long long v = std::strtoll(s.c_str(), &end, 0); // понимает 0x...
    if (!end || *end) return false;
    out = v;
    return true;
}

// Чистое выражение: не вызывает функций, ничего не пишет.
// (бережно: list/struct comprehension, Call и Assign считаем нечистыми)
bool is_pure(const Expr* e) {
    if (!e) return true;
    switch (e->kind) {
        case Expr::Num:
        case Expr::Str:
        case Expr::Bool:
        case Expr::Ident:
        case Expr::Member:
        case Expr::StructLit:
        case Expr::ListLit:
            return true;
        case Expr::Binary:
        case Expr::Unary:
        case Expr::Ternary:
        case Expr::Index:
            for (const auto& k : e->kids)
                if (!is_pure(k.get())) return false;
            return true;
        default:
            return false;
    }
}

// ---------- сбор прочитанных имён ----------

void collect_expr_reads(Expr* e, std::set<std::string>& out) {
    if (!e) return;
    if (e->kind == Expr::Ident) {
        out.insert(e->text);
        return;
    }
    if (e->kind == Expr::StructLit) out.insert(e->field); // Point { ... } — читается тип
    if (e->kind == Expr::Assign) {
        if (e->kids.size() > 1) collect_expr_reads(e->kids[1].get(), out);
        // цель записи сама по себе не чтение, но base у p.x/map[i] — чтение
        if (!e->kids.empty() && e->kids[0] && e->kids[0]->kind != Expr::Ident)
            collect_expr_reads(e->kids[0].get(), out);
        return;
    }
    for (const auto& k : e->kids) collect_expr_reads(k.get(), out);
}

void collect_stmt_reads(Stmt* s, std::set<std::string>& out) {
    if (!s) return;
    collect_expr_reads(s->init.get(), out);
    for (const auto& k : s->kids) collect_expr_reads(k.get(), out);
    for (const auto& b : s->body) collect_stmt_reads(b.get(), out);
    for (const auto& b : s->else_body) collect_stmt_reads(b.get(), out);
    if (s->block) collect_stmt_reads(s->block.get(), out);
    for (const auto& c : s->cases) {
        collect_expr_reads(c.value.get(), out);
        for (const auto& b : c.body) collect_stmt_reads(b.get(), out);
    }
}

bool is_terminator(const Stmt* s) {
    return s && (s->kind == Stmt::Return || s->kind == Stmt::Break ||
                 s->kind == Stmt::Continue);
}

// ---------- основной проход ----------

class Opt {
public:
    Opt(OptReport& rep, const std::string& file) : rep_(rep), file_(file) {}

    bool run(Stmt* root) {
        if (!root) return false;
        bool any = false;
        while (true) {
            reads_.clear();
            collect_stmt_reads(root, reads_);
            changed_ = false;
            changed_ |= walk_body(root->body, /*top_level=*/false);
            if (!changed_) break;
            any = true;
        }
        return any;
    }

private:
    OptReport& rep_;
    std::string file_;
    std::set<std::string> reads_;
    bool changed_ = false;

    void act(size_t line, const std::string& text, OptKind kind) {
        rep_.actions.push_back({file_, line, text, kind});
    }

    bool walk_body(std::vector<StmtP>& body, bool local_scope) {
        bool changed = false;
        for (size_t i = 0; i < body.size();) {
            if (!body[i]) { // мусор от парсера
                body.erase(body.begin() + i);
                changed = changed_ = true;
                continue;
            }
            changed |= walk_stmt(body[i], local_scope);
            if (!body[i]) { // walk_stmt стёр инструкцию
                body.erase(body.begin() + i);
                changed = changed_ = true;
                continue;
            }
            // всё после return/break/continue никогда не выполнится
            if (is_terminator(body[i].get()) && i + 1 < body.size()) {
                for (size_t j = i + 1; j < body.size(); ++j)
                    if (body[j])
                        act(body[j]->span.begin.line,
                            "код после «return» не будет выполнен — удалено",
                            OptKind::Dead);
                rep_.dead += body.size() - (i + 1);
                body.erase(body.begin() + i + 1, body.end());
                changed = changed_ = true;
            }
            ++i;
        }
        return changed;
    }

    // Границы области: у If тело уже развёрнуто в вектор инструкций,
    // у While/ForIn/Loop в body[0] лежит Block — его содержимое и есть вложенная область.
    bool walk_stmt(StmtP& up, bool local_scope) {
        Stmt* s = up.get();
        if (!s) return false;
        bool changed = false;

        switch (s->kind) {
            case Stmt::Let:
            case Stmt::Const: {
                if (s->init) changed |= fold(s->init);
                // локальная переменная, которую никто не читает, и её значение без эффектов
                if (local_scope && !s->name.empty() && !reads_.count(s->name) &&
                    is_pure(s->init.get())) {
                    act(s->name_span.begin.line
                            ? s->name_span.begin.line
                            : s->span.begin.line,
                        "мёртвый код: '" + s->name +
                            "' объявлена, но нигде не читается — удалено",
                        OptKind::Dead);
                    rep_.dead++;
                    up.reset();
                    return true;
                }
                break;
            }

            case Stmt::ExprStmt: {
                changed |= fold(s->kids[0]);
                if (is_pure(s->kids[0].get())) {
                    act(s->span.begin.line,
                        "мёртвый код: выражение «" + show(s->kids[0].get()) +
                            "» ничего не меняет и никому не нужно — удалено",
                        OptKind::Dead);
                    rep_.dead++;
                    up.reset();
                    return true;
                }
                break;
            }

            case Stmt::If: {
                if (s->kids.size()) changed |= fold(s->kids[0]);
                Expr* c = s->kids.size() ? s->kids[0].get() : nullptr;
                if (c && c->kind == Expr::Bool) {
                    bool t = c->text == "true";
                    auto nb = std::make_unique<Stmt>(Stmt::Block);
                    nb->span = s->span;
                    if (t) {
                        act(c->span.begin.line,
                            "условие всегда «истина»: оставлена главная ветка" +
                                std::string(s->else_body.empty() ? ""
                                                                 : ", ветка «else» удалена"),
                            OptKind::Branch);
                        nb->body = std::move(s->body);
                    } else {
                        if (s->else_body.empty()) {
                            act(c->span.begin.line,
                                "условие всегда «ложь»: весь условный блок удалён",
                                OptKind::Branch);
                        } else {
                            act(c->span.begin.line,
                                "условие всегда «ложь»: главная ветка удалена, оставлена «else»",
                                OptKind::Branch);
                            nb->body = std::move(s->else_body);
                        }
                    }
                    rep_.branches++;
                    up = std::move(nb);
                    changed = changed_ = true;
                    return walk_stmt(up, local_scope) || true;
                }
                changed |= walk_body(s->body, true);
                changed |= walk_body(s->else_body, true);
                break;
            }

            case Stmt::While: {
                if (s->kids.size()) changed |= fold(s->kids[0]);
                Expr* c = s->kids.size() ? s->kids[0].get() : nullptr;
                if (c && c->kind == Expr::Bool && c->text == "false") {
                    act(c->span.begin.line,
                        "цикл никогда не выполнится (условие всегда «ложь») — удалён",
                        OptKind::Branch);
                    rep_.branches++;
                    up.reset();
                    return true;
                }
                for (auto& b : s->body) changed |= walk_stmt(b, true);
                break;
            }

            case Stmt::Loop:
                for (auto& b : s->body) changed |= walk_stmt(b, true);
                break;

            case Stmt::ForIn: {
                if (s->kids.size()) changed |= fold(s->kids[0]);
                for (auto& b : s->body) changed |= walk_stmt(b, true);
                break;
            }

            case Stmt::Decide: {
                if (s->kids.size()) changed |= fold(s->kids[0]);
                for (auto& c : s->cases) changed |= walk_body(c.body, true);

                Expr* subj = s->kids.size() ? s->kids[0].get() : nullptr;
                long long v = 0;
                if (subj && subj->kind == Expr::Num && parse_int(subj->text, v)) {
                    long long hit_i = -1, else_i = -1;
                    for (size_t i = 0; i < s->cases.size(); ++i) {
                        const Stmt::Case& c = s->cases[i];
                        long long cv = 0;
                        if (!c.value)
                            else_i = (long long)i;
                        else if (c.value->kind == Expr::Num &&
                                 parse_int(c.value->text, cv) && cv == v) {
                            hit_i = (long long)i;
                            break;
                        }
                    }
                    auto nb = std::make_unique<Stmt>(Stmt::Block);
                    nb->span = s->span;
                    if (hit_i >= 0) {
                        act(subj->span.begin.line,
                            "decide по константе «" + std::to_string(v) +
                                "»: выполнится только ветка weight(" + std::to_string(v) +
                                ") — оставлена она",
                            OptKind::Branch);
                        nb->body = std::move(s->cases[(size_t)hit_i].body);
                        rep_.branches++;
                        up = std::move(nb);
                        changed = changed_ = true;
                        return walk_stmt(up, local_scope) || true;
                    }
                    if (else_i >= 0) {
                        act(subj->span.begin.line,
                            "decide по константе «" + std::to_string(v) +
                                "»: ни одна ветка weight не совпала — оставлена «else»",
                            OptKind::Branch);
                        nb->body = std::move(s->cases[(size_t)else_i].body);
                        rep_.branches++;
                        up = std::move(nb);
                        changed = changed_ = true;
                        return walk_stmt(up, local_scope) || true;
                    }
                    act(subj->span.begin.line,
                        "decide по константе «" + std::to_string(v) +
                            "»: ни одна ветка не подходит — весь decide удалён",
                        OptKind::Branch);
                    rep_.branches++;
                    up.reset();
                    return true;
                }
                break;
            }

            case Stmt::Return:
                if (s->kids.size()) changed |= fold(s->kids[0]);
                break;

            case Stmt::Break:
            case Stmt::Continue:
                break;

            case Stmt::FnDecl:
            case Stmt::AppEntry:
            case Stmt::NodeDecl:
            case Stmt::IsolateDecl: {
                if (s->block) changed |= walk_stmt(s->block, true);
                break;
            }

            case Stmt::Block:
                changed |= walk_body(s->body, true);
                break;

            default: // StructDecl/EnumDecl/Import — нечего оптимизировать
                break;
        }
        return changed;
    }

    // ---- свёртка констант (внизу вверх) ----

    bool fold(ExprP& e) {
        if (!e) return false;
        bool changed = false;
        for (auto& k : e->kids) changed |= fold(k);

        switch (e->kind) {
            case Expr::Binary: {
                Expr* l = e->kids[0].get();
                Expr* r = e->kids[1].get();
                if (!l || !r) break;
                const std::string& op = e->text;

                // оба операнда — числа
                if (l->kind == Expr::Num && r->kind == Expr::Num) {
                    long long a = 0, b = 0;
                    if (!parse_int(l->text, a) || !parse_int(r->text, b)) break;
                    long long v = 0;
                    bool ok = true, res_bool = false, res_true = false;
                    if (op == "+") v = a + b;
                    else if (op == "-") v = a - b;
                    else if (op == "*") v = a * b;
                    else if (op == "/") { if (b == 0) ok = false; else v = a / b; }
                    else if (op == "%") { if (b == 0) ok = false; else v = a % b; }
                    else if (op == "<<") { if (b < 0 || b > 62) ok = false; else v = a << b; }
                    else if (op == ">>") { if (b < 0 || b > 62) ok = false; else v = a >> b; }
                    else if (op == "&") v = a & b;
                    else if (op == "|") v = a | b;
                    else if (op == "^") v = a ^ b;
                    else if (op == "==") { res_bool = true; res_true = (a == b); }
                    else if (op == "!=") { res_bool = true; res_true = (a != b); }
                    else if (op == "<") { res_bool = true; res_true = (a < b); }
                    else if (op == ">") { res_bool = true; res_true = (a > b); }
                    else if (op == "<=") { res_bool = true; res_true = (a <= b); }
                    else if (op == ">=") { res_bool = true; res_true = (a >= b); }
                    else ok = false; // прочие (например «&-») не трогаем
                    if (!ok) break;
                    fold_to(e, res_bool, res_true ? "true" : "false", std::to_string(v));
                    changed = true;
                    break;
                }

                // оба булевых
                if (l->kind == Expr::Bool && r->kind == Expr::Bool) {
                    if (op == "==" || op == "!=" || op == "&&" || op == "||") {
                        bool a = l->text == "true";
                        bool b = r->text == "true";
                        bool t = (op == "==") ? (a == b)
                               : (op == "!=") ? (a != b)
                               : (op == "&&") ? (a && b)
                                              : (a || b);
                        fold_to(e, true, t ? "true" : "false", t ? "true" : "false");
                        changed = true;
                        break;
                    }
                }

                // строки складываем
                if (op == "+" && l->kind == Expr::Str && r->kind == Expr::Str) {
                    std::string shown = show(e.get());
                    ExprP nl = std::make_unique<Expr>(Expr::Str);
                    nl->span = e->span;
                    nl->text = l->text + r->text;
                    act(e->span.begin.line, "свёртка констант: «" + shown + "» → «\"" +
                                                nl->text + "\"»", OptKind::Fold);
                    rep_.folded++;
                    e = std::move(nl);
                    changed = true;
                    break;
                }

                // && и || с константной стороной:
                // правило безопасности — выбрасываем только то, что и так бы не вычислилось.
                if (op == "&&" || op == "||") {
                    bool is_and = op == "&&";
                    if (l->kind == Expr::Bool) {
                        bool t = l->text == "true";
                        // «истина && X» -> X ; «ложь || X» -> X  (X вычислялось всегда)
                        if (t == is_and) {
                            std::string shown = show(e.get());
                            std::string newtext = show(r);
                            ExprP rhs = std::move(e->kids[1]);
                            act(e->span.begin.line,
                                "свёртка констант: «" + shown + "» → «" + newtext +
                                    "» (константа слева ни на что не влияет)",
                                OptKind::Fold);
                            rep_.folded++;
                            e = std::move(rhs);
                            changed = true;
                            break;
                        }
                        // «ложь && X» / «истина || X» -> правая часть не вычислялась
                        if (is_pure(r)) {
                            fold_to(e, true, is_and ? "false" : "true",
                                    is_and ? "false" : "true");
                            changed = true;
                            break;
                        }
                    }
                    if (r->kind == Expr::Bool) {
                        bool t = r->text == "true";
                        // «X && истина» / «X || ложь» -> X (правая — константа без эффектов)
                        if (t == is_and) {
                            std::string shown = show(e.get());
                            std::string newtext = show(l);
                            ExprP lhs = std::move(e->kids[0]);
                            act(e->span.begin.line,
                                "свёртка констант: «" + shown + "» → «" + newtext +
                                    "» (константа справа ни на что не влияет)",
                                OptKind::Fold);
                            rep_.folded++;
                            e = std::move(lhs);
                            changed = true;
                            break;
                        }
                        // «X && ложь» / «X || истина» — результат постоянный,
                        // но левую часть вычислять надо: сворачиваем только если она чистая
                        if (is_pure(l)) {
                            fold_to(e, true, t ? "true" : "false",
                                    is_and ? "false" : "true");
                            changed = true;
                            break;
                        }
                    }
                }
                break;
            }

            case Expr::Unary: {
                Expr* k = e->kids.empty() ? nullptr : e->kids[0].get();
                if (!k) break;
                if (e->text == "!" && k->kind == Expr::Bool) {
                    bool t = k->text != "true";
                    fold_to(e, true, t ? "true" : "false", t ? "true" : "false");
                    changed = true;
                } else if (e->text == "-" && k->kind == Expr::Num) {
                    long long v = 0;
                    if (!parse_int(k->text, v)) break;
                    fold_to(e, false, "", std::to_string(-v));
                    changed = true;
                }
                break;
            }

            case Expr::Ternary: {
                Expr* c = e->kids.size() > 2 ? e->kids[0].get() : nullptr;
                if (!c || c->kind != Expr::Bool) break;
                bool t = c->text == "true";
                std::string shown = show(e.get());
                ExprP branch = std::move(t ? e->kids[1] : e->kids[2]);
                std::string newtext = show(branch.get());
                act(e->span.begin.line,
                    "свёртка констант: «" + shown + "» → «" + newtext +
                        "» (тернарник с постоянным условием)",
                    OptKind::Fold);
                rep_.folded++;
                rep_.branches++;
                e = std::move(branch);
                changed = true;
                break;
            }

            default:
                break;
        }
        return changed;
    }

    // Заменить выражение литералом-результатом.
    void fold_to(ExprP& e, bool res_bool, const std::string& bool_text,
                 const std::string& num_text) {
        std::string shown = show(e.get());
        std::string result = res_bool ? bool_text : num_text;
        ExprP nl = std::make_unique<Expr>(res_bool ? Expr::Bool : Expr::Num);
        nl->span = e->span;
        nl->text = result;
        act(e->span.begin.line, "свёртка констант: «" + shown + "» → «" + result + "»",
            OptKind::Fold);
        rep_.folded++;
        e = std::move(nl);
    }
};

} // namespace

void optimize_file(Stmt* root, const std::string& path, OptReport& rep) {
    Opt(rep, path).run(root);
}

void collect_reads(Stmt* root, std::set<std::string>& out) {
    if (root) collect_stmt_reads(root, out);
}

size_t count_nodes(const Stmt* root) { return count_nodes_impl(root); }
} // namespace kinii
