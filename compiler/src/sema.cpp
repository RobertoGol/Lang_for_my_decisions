// Семантический анализ KINII.
//
// Находит то, что парсер не может знать:
//   1. Неизвестные имена — с подсказкой «может быть, вы имели в виду X»
//      (Левенштейн по всем видимым символам) и ссылкой, где объявлен похожий.
//   2. Несоответствие типов — «let u32 x = "привет";».
//   3. Дубликаты объявлений, число аргументов вызовов, несуществующие поля.
//   4. Полнота decide (ветка else) и пустые decide.
//   5. Системное окружение: sys.*, host.*, console.*, print(...) и т.п.

#include "kinii/sema.h"
#include "kinii/diagnostic.h"

#include <algorithm>
#include <set>

namespace kinii {

Symbol* Scope::find(const std::string& name) {
    Scope* s = this;
    while (s) {
        auto it = s->syms.find(name);
        if (it != s->syms.end()) return &it->second;
        s = s->parent;
    }
    return nullptr;
}

const Symbol* Scope::find(const std::string& name) const {
    const Scope* s = this;
    while (s) {
        auto it = s->syms.find(name);
        if (it != s->syms.end()) return &it->second;
        s = s->parent;
    }
    return nullptr;
}

namespace {

// ---------- размер типов (байты) — пригодится оптимизатору ----------

uint32_t type_size_of(const TypeExpr* t) {
    if (!t) return 0;
    switch (t->kind) {
        case TypeExpr::Named: {
            if (t->name == "u8") return 1;
            if (t->name == "u16") return 2;
            if (t->name == "u32") return 4;
            if (t->name == "u64") return 8;
            if (t->name == "bool") return 1;
            if (t->name == "ptr" || t->name == "v_addr") return 8;
            if (t->name == "v_word") return 4;
            return 0;
        }
        case TypeExpr::Array:
            return t->array_len * type_size_of(t->elem.get());
        default:
            return 0;
    }
}

std::string type_text_of(const TypeExpr* t) {
    if (!t) return "<выведен>";
    switch (t->kind) {
        case TypeExpr::Named:
            return t->name;
        case TypeExpr::Cell:
            return t->name.empty() ? "cell" : "cell:" + t->name;
        case TypeExpr::List:
            return "list<" + type_text_of(t->elem.get()) + ">";
        case TypeExpr::Array:
            return type_text_of(t->elem.get()) + "[" + std::to_string(t->array_len) + "]";
    }
    return "?";
}

// Точка-путь вида "console.print" — пусто, если это не цепочка имён через точку.
std::string dotted_path(const Expr* e) {
    if (!e) return "";
    if (e->kind == Expr::Ident) return e->text;
    if (e->kind == Expr::Member && !e->kids.empty()) {
        std::string base = dotted_path(e->kids[0].get());
        if (base.empty()) return "";
        return base + "." + e->field;
    }
    return "";
}

size_t lev(const std::string& a, const std::string& b) {    const size_t n = a.size(), m = b.size();
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

// Имена, приходящие «из платформы» — не требуют объявления в коде.
bool is_known_root(const std::string& name) {
    static const std::set<std::string> roots = {
        "sys", "host", "console", "mem", "net", "hw", "telemetry", "sql", "env",
        "hash", "fwr", "compiler", "bridge", "logic",
        "print", "range", "len",
    };
    return roots.count(name) != 0;
}

class SemaVisitor {
public:
    SemaVisitor(DiagnosticEngine& dg, FileId fid, FileSema& out, SemaResult& res)
        : dg_(dg), fid_(fid), out_(out), res_(res) {}

    void walk(const StmtP& prog) { walk_stmt(prog.get()); }

private:
    DiagnosticEngine& dg_;
    FileId fid_;
    FileSema& out_;
    SemaResult& res_;
    std::vector<std::unique_ptr<Scope>> scopes_;
    int loops_ = 0; // глубина циклов: break/continue живут только в них

    Scope* cur() { return scopes_.back().get(); }

    void push_scope() {
        auto s = std::make_unique<Scope>();
        s->parent = scopes_.empty() ? nullptr : scopes_.back().get();
        scopes_.push_back(std::move(s));
    }
    void pop_scope() { if (!scopes_.empty()) scopes_.pop_back(); }

    void declare(const Symbol& s) {
        auto& syms = cur()->syms;
        auto it = syms.find(s.name);
        if (it != syms.end()) {
            auto b = dg_.error("K2001", "имя '" + s.name + "' уже объявлено", s.span);
            if (it->second.file == fid_ && it->second.span.begin.line)
                b.label(it->second.span, "первое объявление здесь");
            b.hint("переименуйте одно из объявлений");
            b.emit();
            return;
        }
        syms[s.name] = s;
        if (scopes_.size() == 1) out_.top_decls[s.name] = s; // видно другим файлам
    }

    void reference(const std::string& name, const Span& sp) {
        if (cur()->find(name)) return;
        if (is_known_root(name)) return;

        // Поиск в других файлах проекта.
        Symbol* gsym = find_global(name);
        if (gsym) {
            if (is_imported(gsym->file) || res_.project_wide) {
                gsym->used = true; // пригодится оптимизатору
                return;            // имя из другого файла проекта — корректно
            }
            std::string other;
            if (gsym->file < res_.files.size()) other = res_.files[gsym->file].path;
            auto b =
                dg_.error("K2011", "имя '" + name + "' объявлено в файле " + other +
                                       ", но этот файл не импортирован сюда", sp);
            b.label(gsym->span, "'" + name + "' объявлен в другом файле");
            b.hint("добавьте в начало файла: import \"" + other + "\";");
            b.fix("как должно выглядеть:", "import \"" + other + "\";");
            b.emit();
            return;
        }

        // Кандидаты: всё видимое из текущей области и объявления других файлов.
        std::vector<std::pair<std::string, const Symbol*>> visible;
        for (Scope* s = cur(); s; s = s->parent) {
            for (const auto& kv : s->syms) visible.push_back({kv.first, &kv.second});
            break; // кандидатов берём только из текущей области — иначе шум
        }
        for (const auto& kv : res_.global)
            if (kv.second.file != fid_) visible.push_back({kv.first, &kv.second});

        std::string best;
        const Symbol* best_sym = nullptr;
        size_t bd = SIZE_MAX;
        for (const auto& cand : visible) {
            size_t d = lev(name, cand.first);
            if (d < bd) {
                bd = d;
                best = cand.first;
                best_sym = cand.second;
            }
        }

        res_.unknown_symbols++;
        auto b = dg_.error("K2002", "неизвестное имя '" + name + "'", sp);
        if (bd <= 2 && !best.empty()) {
            b.suggest(best);
            if (best_sym && best_sym->span.begin.line)
                b.label(best_sym->span, "'" + best + "' объявлен здесь");
        } else if (visible.empty()) {
            b.hint("имя нигде не объявлено — создайте переменную: let " + name + " = 0;");
        } else {
            std::string r;
            for (size_t i = 0; i < visible.size() && i < 12; ++i) {
                if (i) r += ", ";
                r += visible[i].first;
            }
            if (visible.size() > 12) r += ", ...";
            b.hint("видимые имена в этой точке: " + r);
        }
        b.emit();
    }

    Symbol* find_global(const std::string& name) {
        for (auto& kv : res_.global)
            if (kv.first == name && kv.second.file != fid_) return &kv.second;
        return nullptr;
    }

    bool is_imported(FileId f) const {
        for (FileId id : out_.imported_ids)
            if (id == f) return true;
        return false;
    }

    // Грубая проверка типов при присваивании.
    void check_assign_type(const std::string& dst, const std::string& src, const Span& sp) {
        if (dst.empty() || src.empty()) return;
        if (dst == src) return;

        static const std::set<std::string> nums = {"u8", "u16", "u32", "u64", "v_word"};
        bool dst_num = nums.count(dst) != 0, src_num = nums.count(src) != 0;
        if (dst_num && src_num) return; // числа между собой: усечения ловим позже
        if (dst.rfind("cell", 0) == 0 || src.rfind("cell", 0) == 0) return;
        if (dst.rfind("<выведен>", 0) == 0 || src.rfind("<выведен>", 0) == 0) return;

        auto b = dg_.error("K2003",
                           "несоответствие типов: нельзя записать '" + src + "' в '" + dst + "'",
                           sp);
        b.hint("'" + dst + "' хранит совсем другое, чем даёт '" + src + "'");
        if (dst == "u32" && src == "string")
            b.hint("строку нельзя записать в целое число — типы должны совпадать");
        b.emit();
    }

    // Тип выражения (грубая оценка для сообщений).
    std::string expr_type(const Expr* e) {
        if (!e) return "";
        switch (e->kind) {
            case Expr::Num: return "u32";
            case Expr::Str: return "string";
            case Expr::Bool: return "bool";
            case Expr::Ident: {
                const Symbol* s = cur()->find(e->text);
                return s ? s->type_text : "";
            }
            case Expr::Binary: {
                const std::string& op = e->text;
                if (op == "==" || op == "!=" || op == "<" || op == ">" || op == "<=" ||
                    op == ">=" || op == "&&" || op == "||")
                    return "bool";
                if (op == "+" && expr_type(e->kids[0].get()) == "string") return "string";
                return "u32";
            }
            default: return "";
        }
    }

    void walk_stmt(const Stmt* st) {
        if (!st) return;

        switch (st->kind) {
            case Stmt::Import:
                out_.imports.push_back(st->name);
                break;

            case Stmt::Let:
            case Stmt::Const: {
                Symbol s;
                s.kind = (st->kind == Stmt::Const) ? Symbol::Const : Symbol::Var;
                s.name = st->name;
                s.type_text = type_text_of(st->type.get());
                s.type_size = type_size_of(st->type.get());
                s.is_const = (st->kind == Stmt::Const);
                s.file = fid_;
                s.span = st->name_span.file != kNoFile ? st->name_span : st->span;
                declare(s);

                if (st->init) {
                    walk_expr(st->init.get());
                    if (st->type) {
                        std::string src = expr_type(st->init.get());
                        std::string dst = type_text_of(st->type.get());
                        if (!src.empty() && !dst.empty())
                            check_assign_type(dst, src, st->init->span);
                    }
                }
                break;
            }

            case Stmt::FnDecl: {
                Symbol s;
                s.kind = Symbol::Fn;
                s.name = st->name;
                s.type_text = st->ret ? type_text_of(st->ret.get()) : "void";
                s.is_pub = st->is_pub;
                s.param_count = (int)st->params.size();
                s.file = fid_;
                s.span = st->name_span.file != kNoFile ? st->name_span : st->span;
                declare(s);

                push_scope();
                for (const auto& p : st->params) {
                    Symbol ps;
                    ps.kind = Symbol::Param;
                    ps.name = p.name;
                    ps.type_text = type_text_of(p.type.get());
                    ps.file = fid_;
                    ps.span = p.span;
                    declare(ps);
                }
                int saved_loops = loops_;
                loops_ = 0; // у функции свои циклы — break не перепрыгивает границы
                if (st->block) walk_stmt(st->block.get());
                loops_ = saved_loops;
                pop_scope();
                break;
            }

            case Stmt::StructDecl: {
                Symbol s;
                s.kind = Symbol::Struct;
                s.name = st->name;
                s.type_text = "struct";
                s.file = fid_;
                s.span = st->name_span.file != kNoFile ? st->name_span : st->span;
                declare(s);
                for (const auto& f : st->fields) {
                    Symbol fs;
                    fs.kind = Symbol::Field;
                    fs.name = st->name + "." + f.name;
                    fs.type_text = type_text_of(f.type.get());
                    fs.file = fid_;
                    fs.span = f.span;
                    declare(fs);
                }
                break;
            }

            case Stmt::EnumDecl: {
                Symbol s;
                s.kind = Symbol::Enum;
                s.name = st->name;
                s.type_text = "enum";
                s.file = fid_;
                s.span = st->name_span.file != kNoFile ? st->name_span : st->span;
                declare(s);
                break;
            }

            case Stmt::NodeDecl:
            case Stmt::IsolateDecl: {
                Symbol s;
                s.kind = Symbol::Struct;
                s.name = st->name;
                s.type_text = (st->kind == Stmt::NodeDecl) ? "node" : "isolate";
                s.file = fid_;
                s.span = st->name_span.file != kNoFile ? st->name_span : st->span;
                declare(s);
                push_scope();
                if (st->block) walk_stmt(st->block.get());
                pop_scope();
                // Функции узла — функции проекта: поднимаем их на верхний уровень
                // (кодогенерация делает то же самое — честная пара).
                if (st->block) {
                    for (const auto& inner : st->block->body) {
                        if (!inner || inner->kind != Stmt::FnDecl) continue;
                        Symbol f;
                        f.kind = Symbol::Fn;
                        f.name = inner->name;
                        f.type_text =
                            inner->ret ? type_text_of(inner->ret.get()) : "void";
                        f.is_pub = inner->is_pub;
                        f.param_count = (int)inner->params.size();
                        f.file = fid_;
                        f.span = inner->name_span.file != kNoFile ? inner->name_span
                                                                  : inner->span;
                        declare(f); // до имени дотянется и этот файл, и весь проект
                    }
                }
                break;
            }

            case Stmt::AppEntry: {
                push_scope();
                if (st->block) walk_stmt(st->block.get());
                pop_scope();
                break;
            }

            case Stmt::Block: {
                push_scope();
                for (const auto& b : st->body) walk_stmt(b.get());
                pop_scope();
                break;
            }

            case Stmt::If: {
                if (!st->kids.empty()) walk_expr(st->kids[0].get());
                push_scope();
                for (const auto& b : st->body) walk_stmt(b.get());
                pop_scope();
                push_scope();
                for (const auto& b : st->else_body) walk_stmt(b.get());
                pop_scope();
                break;
            }

            case Stmt::While: {
                if (!st->kids.empty()) walk_expr(st->kids[0].get());
                push_scope();
                loops_++;
                for (const auto& b : st->body) walk_stmt(b.get());
                loops_--;
                pop_scope();
                break;
            }

            case Stmt::Loop: {
                push_scope();
                loops_++;
                for (const auto& b : st->body) walk_stmt(b.get());
                loops_--;
                pop_scope();
                break;
            }

            case Stmt::ForIn: {
                if (!st->kids.empty()) walk_expr(st->kids[0].get());
                push_scope();
                Symbol v;
                v.kind = Symbol::Var;
                v.name = st->name;
                v.type_text = "<элемент списка>";
                v.file = fid_;
                v.span = st->span;
                declare(v);
                loops_++;
                for (const auto& b : st->body) walk_stmt(b.get());
                loops_--;
                pop_scope();
                break;
            }

            case Stmt::Decide: {
                if (!st->kids.empty()) walk_expr(st->kids[0].get());
                if (st->cases.empty()) {
                    dg_.warning("K2004", "'decide' без веток — он ничего не решает", st->span)
                        .hint("добавьте ветку: weight(значение) => { ... } или else => { ... }")
                        .emit();
                }
                bool has_else = false;
                for (const auto& c : st->cases) {
                    if (!c.value) has_else = true;
                    walk_expr(c.value.get());
                    push_scope();
                    for (const auto& b : c.body) walk_stmt(b.get());
                    pop_scope();
                }
                if (!has_else && !st->cases.empty())
                    dg_.warning(
                        "K2005",
                        "в 'decide' нет ветки 'else' — если ни один weight не совпал, "
                        "выполнение молча перейдёт дальше",
                        st->span)
                        .hint("добавьте: else => { ... } — или оставьте, если «промах» это норма")
                        .emit();
                break;
            }

            case Stmt::Return:
                if (!st->kids.empty()) walk_expr(st->kids[0].get());
                break;

            case Stmt::Break:
            case Stmt::Continue:
                // честность: break/continue выходят из ближайшего цикла.
                // decide — ветвление, а не цикл; break внутри него уходит в цикл.
                if (loops_ == 0) {
                    dg_.error(st->kind == Stmt::Break ? "K2013" : "K2014",
                              std::string(st->kind == Stmt::Break ? "break" : "continue") +
                                  " не в цикле — он имеет смысл только внутри "
                                  "while/loop/for",
                              st->span)
                        .hint(st->kind == Stmt::Break
                                  ? "выйти из decide нельзя — он просто выбирает ветку"
                                  : "continue тоже работает только в цикле")
                        .emit();
                }
                break;

            case Stmt::ExprStmt:
                if (!st->kids.empty()) walk_expr(st->kids[0].get());
                break;

            default:
                break;
        }
    }

    void walk_expr(const Expr* e) {
        if (!e) return;
        switch (e->kind) {
            case Expr::Ident:
                reference(e->text, e->span);
                break;

            case Expr::Assign: {
                // Цель присваивания: переменная — имя; поле/элемент — обычный проход,
                // который проверит существование поля.
                const Expr* dst = e->kids[0].get();
                if (dst->kind == Expr::Ident) {
                    if (!is_known_root(dst->text)) reference(dst->text, dst->span);
                } else {
                    walk_expr(dst); // Member: проверка поля; Index: проверка базы
                }
                walk_expr(e->kids[1].get());
                break;
            }

            case Expr::Call: {
                walk_expr(e->kids[0].get());
                const Expr* callee = e->kids[0].get();
                if (callee->kind == Expr::Ident) {
                    const Symbol* s = cur()->find(callee->text);
                    if (!s) s = find_global(callee->text);
                    if (s && s->kind == Symbol::Fn && s->param_count >= 0 &&
                        (s->file == fid_ || is_imported(s->file) || res_.project_wide)) {
                        size_t nargs = e->kids.size() - 1;
                        if (nargs != (size_t)s->param_count) {
                            auto b = dg_.error(
                                "K2006",
                                "функция '" + callee->text + "' принимает " +
                                    std::to_string(s->param_count) +
                                    " аргумент(ов), передано " +
                                    std::to_string(nargs),
                                e->span);
                            b.label(s->span, "объявлена здесь");
                            b.hint("сверьтесь со списком параметров в объявлении");
                            b.emit();
                        }
                    }
                }
                // Системные вызовы бэкенда: проверяем число аргументов честно.
                if (callee->kind == Expr::Member) {
                    std::string path = dotted_path(callee);
                    size_t nargs = e->kids.size() - 1;
                    auto wrong = [&](const std::string& want) {
                        dg_.error("K2006",
                                  "системный вызов '" + path + "' принимает " +
                                      want + ", передано " + std::to_string(nargs),
                                  e->span)
                            .hint("сверьтесь со списком системных вызовов в SPEC.md")
                            .emit();
                    };
                    if (path == "sys.exit" && nargs != 1)
                        wrong("1 аргумент (код выхода)");
                    else if (path == "sys.time.now" && nargs != 0)
                        wrong("0 аргументов");
                    else if (path == "sys.rnd.next" && nargs != 1)
                        wrong("1 аргумент (верхняя граница)");
                    else if ((path == "console.read_input" ||
                              path == "console.read_line") &&
                             nargs > 1)
                        wrong("0–1 аргумент (необязательная подсказка)");
                }
                for (size_t i = 1; i < e->kids.size(); ++i) walk_expr(e->kids[i].get());
                break;
            }

            case Expr::Member: {
                const Expr* obj = e->kids[0].get();
                if (obj->kind == Expr::Ident) {
                    const Symbol* tsym = cur()->find(obj->text);
                    // Имя структуры: либо сам тип (Point.x), либо переменная этого типа (p.x)
                    std::string sname;
                    if (tsym) {
                        if (tsym->kind == Symbol::Struct) {
                            sname = obj->text;
                        } else if (tsym->kind == Symbol::Var || tsym->kind == Symbol::Const) {
                            auto it = out_.top_decls.find(tsym->type_text);
                            if (it != out_.top_decls.end() && it->second.kind == Symbol::Struct)
                                sname = tsym->type_text;
                        }
                    }
                    if (!sname.empty()) {
                        std::string prefix = sname + ".";
                        const Symbol* fsym = nullptr;
                        for (const auto& kv : out_.top_decls)
                            if (kv.first == prefix + e->field) fsym = &kv.second;
                        if (!fsym) {
                            std::string best;
                            size_t bd = SIZE_MAX;
                            for (const auto& kv : out_.top_decls) {
                                if (kv.first.rfind(prefix, 0) != 0) continue;
                                size_t d = lev(e->field, kv.first.substr(prefix.size()));
                                if (d < bd) {
                                    bd = d;
                                    best = kv.first;
                                }
                            }
                            auto b = dg_.error(
                                "K2007", "в структуре '" + sname + "' нет поля '" + e->field + "'",
                                e->span);
                            if (!best.empty() && bd <= 2) {
                                b.suggest(best.substr(prefix.size()));
                                const Symbol* bs = cur()->find(best);
                                if (bs && bs->file == fid_)
                                    b.label(bs->span, "похожее поле объявлено здесь");
                                std::string have;
                                for (const auto& kv : out_.top_decls)
                                    if (kv.first.rfind(prefix, 0) == 0)
                                        have += (have.empty() ? "" : ", ") +
                                                kv.first.substr(prefix.size());
                                if (!have.empty()) b.hint("есть поля: " + have);
                            } else {
                                std::string have;
                                for (const auto& kv : out_.top_decls)
                                    if (kv.first.rfind(prefix, 0) == 0)
                                        have += (have.empty() ? "" : ", ") +
                                                kv.first.substr(prefix.size());
                                b.hint(have.empty() ? "у структуры нет полей"
                                                    : "есть поля: " + have);
                            }
                            b.emit();
                        }
                    }
                }
                walk_expr(obj);
                break;
            }

            case Expr::StructLit: {
                // Проверяем, что перечисленные поля есть у структуры.
                const Symbol* tsym = cur()->find(e->field);
                if (tsym && tsym->kind == Symbol::Struct &&
                    e->fields.size() == e->kids.size()) {
                    std::string prefix = e->field + ".";
                    for (size_t k = 0; k < e->fields.size(); ++k) {
                        const std::string& fname = e->fields[k];
                        bool ok = false;
                        for (const auto& kv : out_.top_decls)
                            if (kv.first == prefix + fname) ok = true;
                        if (ok) continue;
                        std::string best;
                        size_t bd = SIZE_MAX;
                        for (const auto& kv : out_.top_decls) {
                            if (kv.first.rfind(prefix, 0) != 0) continue;
                            size_t d = lev(fname, kv.first.substr(prefix.size()));
                            if (d < bd) {
                                bd = d;
                                best = kv.first;
                            }
                        }
                        auto b = dg_.error(
                            "K2008",
                            "в структуре '" + e->field + "' нет поля '" + fname + "'",
                            e->kids[k]->span);
                        if (!best.empty() && bd <= 2)
                            b.suggest(best.substr(prefix.size()));
                        std::string have;
                        for (const auto& kv : out_.top_decls)
                            if (kv.first.rfind(prefix, 0) == 0)
                                have +=
                                    (have.empty() ? "" : ", ") + kv.first.substr(prefix.size());
                        b.hint(have.empty() ? "у структуры нет полей" : "есть поля: " + have);
                        b.emit();
                    }
                }
                for (const auto& k : e->kids) walk_expr(k.get());
                break;
            }

            case Expr::Comprehension: {
                // Переменная comprehension (`[i for i in xs ...]`) видна
                // только внутри самого comprehension.
                push_scope();
                Symbol v;
                v.kind = Symbol::Var;
                v.name = e->text;
                v.type_text = "<элемент списка>";
                v.file = fid_;
                v.span = e->span;
                cur()->syms[v.name] = v; // без declare(): перекрытие внешнего имени — норма
                if (!e->kids.empty()) walk_expr(e->kids[0].get());      // элемент
                if (e->kids.size() > 1) walk_expr(e->kids[1].get());    // источник
                if (e->kids.size() > 2) walk_expr(e->kids[2].get());    // условие
                pop_scope();
                break;
            }

            case Expr::Binary:
            case Expr::Unary:
            case Expr::Ternary:
            case Expr::Index:
            case Expr::ListLit:
                for (const auto& k : e->kids) walk_expr(k.get());
                break;

            case Expr::Num:
            case Expr::Str:
            case Expr::Bool:
                break;
        }
    }
};

} // namespace

void run_sema(FileId fid, const StmtP& ast, DiagnosticEngine& dg, SemaResult& res) {
    if (res.files.size() <= fid) res.files.resize(fid + 1);
    FileSema& fs = res.files[fid];
    fs.file = fid;
    SemaVisitor v(dg, fid, fs, res);
    v.walk(ast);
    // Верхнеуровневые объявления файла становятся видимыми всему проекту.
    for (const auto& kv : fs.top_decls) res.global.insert(kv);
}

} // namespace kinii
