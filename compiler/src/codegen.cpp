// Кодогенератор: AST -> C++20.
// Две простые гарантии: 1) мы выводим только то, что уже прошло семантику
// и оптимизацию; 2) если фича ещё не описана в бэкенде — честная ошибка K3001,
// а не «молча напечатаем что-то не то».

#include "kinii/codegen.h"
#include "kinii/ast.h"
#include "kinii/diagnostic.h"

#include <iostream>
#include <sstream>

namespace kinii {

namespace {

// Встроенный минимальный рантайм: print/len.
// مواد u8 печатается числом (у std::cout uint8_t печатался бы как символ).
const char* kRuntime = R"RT(
// Стандартная библиотека C++ (runtime KINII генерирует сам).
#include <array>
#include <cstdint>
#include <iostream>
#include <string>
#include <type_traits>
#include <vector>

namespace kn {
template <class T>
void print(const T& v) {
    if constexpr (std::is_integral_v<T> && sizeof(T) == 1)
        std::cout << (int)v;
    else
        std::cout << v;
    std::cout << std::endl;
}
template <class C>
auto len(const C& c) -> decltype(c.size()) {
    return c.size();
}
} // namespace kn

// «строка + число» — это склейка, а не арифметика указателей.
template <class T>
inline std::enable_if_t<std::is_integral<T>::value, std::string>
operator+(const std::string& s, T v) {
    return s + std::to_string(v);
}

template <class T>
inline std::enable_if_t<std::is_integral<T>::value, std::string>
operator+(T v, const std::string& s) {
    return std::to_string(v) + s;
}
)RT";

std::string cxx_escape(const std::string& s) {
    std::string r;
    for (unsigned char c : s) {
        switch (c) {
            case '"': r += "\\\""; break;
            case '\\': r += "\\\\"; break;
            case '\n': r += "\\n"; break;
            case '\t': r += "\\t"; break;
            case '\r': r += "\\r"; break;
            default: r += (char)c;
        }
    }
    return r;
}

std::string builtin_type(const std::string& n) {
    if (n == "u8") return "uint8_t";
    if (n == "u16") return "uint16_t";
    if (n == "u32") return "uint32_t";
    if (n == "u64") return "uint64_t";
    if (n == "bool") return "bool";
    if (n == "string") return "std::string";
    if (n == "ptr") return "void*";
    if (n == "v_word") return "uint64_t";
    if (n == "v_addr") return "void*";
    return n; // имена структур проходят как есть
}

std::string join_comma(const std::vector<std::string>& v) {
    std::string r;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) r += ", ";
        r += v[i];
    }
    return r;
}

} // namespace

CodeGen::CodeGen(DiagnosticEngine& dg, std::ostream& os) : dg_(dg), os_(os) {}

void CodeGen::error_unsupported(const char* code, const std::string& what, const Span& sp) {
    saw_error_ = true;
    dg_.error(code, what, sp)
        .hint("эта конструкция ещё не переведена в C++ бэкендом")
        .emit();
}

void CodeGen::out_indent() {
    for (int i = 0; i < ind_; ++i) os_ << "    ";
}

void CodeGen::line(const std::string& text) {
    if (text.empty()) {
        os_ << "\n";
        return;
    }
    out_indent();
    os_ << text << "\n";
}

void CodeGen::begin() {
    os_ << "// Сгенерировано kinic из KINII v3 — является выходом компилятора.\n";
    os_ << "// Правки вручную потеряются: следующий запуск перезапишет файл.\n";
    os_ << kRuntime << "\n";
}

std::string CodeGen::type_str(const TypeExpr* t) const {
    if (!t) return "auto";
    switch (t->kind) {
        case TypeExpr::Named:
            return builtin_type(t->name);
        case TypeExpr::Cell:
            return type_str(t->elem.get()); // модификатор cell на выход не влияет
        case TypeExpr::List:
            return "std::vector<" + type_str(t->elem.get()) + ">";
        case TypeExpr::Array:
            return "std::array<" + type_str(t->elem.get()) + ", " +
                   std::to_string(t->array_len) + ">";
    }
    return "auto";
}

// ---------- выражения ----------

std::string CodeGen::gen_expr(const Expr* e) {
    if (!e) return "";
    switch (e->kind) {
        case Expr::Num:
        case Expr::Bool:
        case Expr::Ident:
            return e->text;
        case Expr::Str:
            return "std::string(\"" + cxx_escape(e->text) + "\")";
        case Expr::Binary: {
            const Expr* l = e->kids[0].get();
            const Expr* r = e->kids[1].get();
            if (!l || !r) return "";
            return "(" + gen_expr(l) + " " + e->text + " " + gen_expr(r) + ")";
        }
        case Expr::Unary: {
            if (e->kids.empty() || !e->kids[0]) return "";
            std::string op = e->text == "!" ? "!" : e->text;
            return "(" + op + gen_expr(e->kids[0].get()) + ")";
        }
        case Expr::Ternary: {
            if (e->kids.size() < 3) return "";
            return "(" + gen_expr(e->kids[0].get()) + " ? " +
                   gen_expr(e->kids[1].get()) + " : " + gen_expr(e->kids[2].get()) +
                   ")";
        }
        case Expr::Assign: {
            if (e->kids.size() < 2) return "";
            return "(" + gen_expr(e->kids[0].get()) + " = " +
                   gen_expr(e->kids[1].get()) + ")";
        }
        case Expr::Call: {
            const Expr* callee = e->kids[0].get();
            if (!callee) return "";

            // Встроенные функции языка -> рантайм kn::.
            if (callee->kind == Expr::Ident) {
                if (callee->text == "print") {
                    std::vector<std::string> args;
                    for (size_t i = 1; i < e->kids.size(); ++i)
                        args.push_back(gen_expr(e->kids[i].get()));
                    return "kn::print(" + join_comma(args) + ")";
                }
                if (callee->text == "len") {
                    if (e->kids.size() < 2) return "0";
                    return "kn::len(" + gen_expr(e->kids[1].get()) + ")";
                }
            }
            if (callee->kind == Expr::Member) {
                // что-то вида console.log / sys.* — в этом бэкенде нет
                std::string full =
                    (callee->kids[0] ? callee->kids[0]->text : "?") + "." + callee->field;
                error_unsupported("K3001",
                                  "встроенная операция '" + full +
                                      "' не поддерживается в бэкенде",
                                  callee->span);
                return "0";
            }

            std::vector<std::string> args;
            for (size_t i = 1; i < e->kids.size(); ++i)
                args.push_back(gen_expr(e->kids[i].get()));
            return gen_expr(callee) + "(" + join_comma(args) + ")";
        }
        case Expr::Member: {
            // DataType.U32 -> DataType::U32 (перечисления)
            if (e->kids[0] && e->kids[0]->kind == Expr::Ident &&
                enums_.count(e->kids[0]->text))
                return e->kids[0]->text + "::" + e->field;
            return gen_expr(e->kids[0].get()) + "." + e->field;
        }
        case Expr::Index:
            return gen_expr(e->kids[0].get()) + "[" + gen_expr(e->kids[1].get()) + "]";

        case Expr::ListLit: {
            // работает как инициализация {a, b} в контексте list<T>
            std::vector<std::string> items;
            for (const auto& k : e->kids)
                if (k) items.push_back(gen_expr(k.get()));
            return "{" + join_comma(items) + "}";
        }

        case Expr::StructLit: {
            // Имена полей -> позиционные значения в порядке объявления структуры.
            auto it = struct_fields_.find(e->field);
            std::vector<std::string> vals(e->kids.size());
            if (it != struct_fields_.end()) {
                for (size_t i = 0; i < e->fields.size() && i < e->kids.size(); ++i) {
                    const std::string& fname = e->fields[i];
                    long long pos = -1;
                    for (size_t j = 0; j < it->second.size(); ++j)
                        if (it->second[j] == fname) { pos = (long long)j; break; }
                    if (pos >= 0 && pos < (long long)vals.size())
                        vals[(size_t)pos] = gen_expr(e->kids[i].get());
                    else
                        vals[i] = gen_expr(e->kids[i].get());
                }
            }
            for (size_t i = 0; i < vals.size(); ++i)
                if (vals[i].empty()) vals[i] = gen_expr(i < e->kids.size() ? e->kids[i].get() : nullptr);
            return e->field + "{" + join_comma(vals) + "}";
        }

        case Expr::Comprehension: {
            // [elem for v in src if cond] -> лямбда с вектором
            const Expr* elem = e->kids[0].get();
            const Expr* src = e->kids[1].get();
            const Expr* cond = e->kids.size() > 2 ? e->kids[2].get() : nullptr;
            std::ostringstream ss;
            ss << "[&]{\n"
               << "                std::vector<typename std::decay_t<decltype("
               << gen_expr(src) << ")>::value_type> r;\n"
               << "                for (auto& " << e->text << " : " << gen_expr(src)
               << ") {\n"
               << "                    if (" << (cond ? gen_expr(cond) : "true")
               << ") r.push_back(" << gen_expr(elem) << ");\n"
               << "                }\n"
               << "                return r;\n"
               << "            }()";
            return ss.str();
        }
    }
    return "";
}

// ---------- инструкции ----------

void CodeGen::gen_body(const std::vector<StmtP>& body) {
    for (const auto& s : body)
        if (s) gen_stmt(s.get());
}

void CodeGen::gen_stmt(Stmt* s) {
    if (!s) return;

    switch (s->kind) {
        case Stmt::Block: {
            line("{");
            ind_++;
            gen_body(s->body);
            ind_--;
            line("}");
            break;
        }

        case Stmt::If: {
            std::string cond =
                s->kids.size() ? gen_expr(s->kids[0].get()) : "false";
            line("if (" + cond + ") {");
            ind_++;
            gen_body(s->body);
            ind_--;
            if (s->else_body.empty()) {
                line("}");
                break;
            }
            // ветка else — это ещё один if: пишем «} else» и отдаём ему строку целиком
            const Stmt* e0 = s->else_body.size() == 1 && s->else_body[0]
                                 ? s->else_body[0].get()
                                 : nullptr;
            if (e0 && e0->kind == Stmt::If) {
                line("} else");
                gen_stmt(s->else_body[0].get());
                break;
            }
            line("} else {");
            ind_++;
            gen_body(s->else_body);
            ind_--;
            line("}");
            break;
        }

        case Stmt::While: {
            std::string cond = s->kids.size() ? gen_expr(s->kids[0].get()) : "false";
            line("while (" + cond + ") {");
            ind_++;
            if (!s->body.empty() && s->body[0] && s->body[0]->kind == Stmt::Block)
                gen_body(s->body[0]->body);
            else
                gen_body(s->body);
            ind_--;
            line("}");
            break;
        }

        case Stmt::Loop: {
            line("for (;;) {");
            ind_++;
            if (!s->body.empty() && s->body[0] && s->body[0]->kind == Stmt::Block)
                gen_body(s->body[0]->body);
            else
                gen_body(s->body);
            ind_--;
            line("}");
            break;
        }

        case Stmt::ForIn: {
            std::string src = s->kids.size() ? gen_expr(s->kids[0].get()) : "";
            line("for (auto& " + s->name + " : " + src + ") {");
            ind_++;
            if (!s->body.empty() && s->body[0] && s->body[0]->kind == Stmt::Block)
                gen_body(s->body[0]->body);
            else
                gen_body(s->body);
            ind_--;
            line("}");
            break;
        }

        case Stmt::Decide: {
            std::string subj = s->kids.size() ? gen_expr(s->kids[0].get()) : "";
            line("switch (" + subj + ") {");
            ind_++;
            for (const auto& c : s->cases) {
                if (c.value) {
                    const Expr* v = c.value.get();
                    if (v->kind == Expr::Str) {
                        error_unsupported(
                            "K3001", "decide работает по числам и перечислениям, "
                                     "не по строкам", v->span);
                    }
                    line("case " + gen_expr(v) + ": {");
                } else {
                    line("default: {");
                }
                ind_++;
                gen_body(c.body);
                ind_--;
                line("    break;");
                line("}");
            }
            ind_--;
            line("}");
            break;
        }

        case Stmt::Let:
        case Stmt::Const: {
            const std::string kw = s->kind == Stmt::Const ? "const " : "";
            std::string t = type_str(s->type.get());
            if (s->type) {
                line(kw + t + " " + s->name + " = " +
                     (s->init ? gen_expr(s->init.get()) : "{}") + ";");
            } else if (s->init && s->init->kind == Expr::ListLit) {
                // список без типа: выведем тип из первого элемента
                std::string elem = "int64_t";
                if (!s->init->kids.empty() && s->init->kids[0]) {
                    const Expr* f = s->init->kids[0].get();
                    if (f->kind == Expr::Str) elem = "std::string";
                    else if (f->kind == Expr::Bool) elem = "bool";
                    else if (f->kind == Expr::Num) elem = "int64_t";
                    else
                        elem = "decltype(" + gen_expr(f) + ")";
                }
                line(kw + "std::vector<" + elem + "> " + s->name + " = " +
                     gen_expr(s->init.get()) + ";");
            } else if (s->init && s->init->kind == Expr::Comprehension) {
                line(kw + "auto " + s->name + " = " + gen_expr(s->init.get()) + ";");
            } else {
                line(kw + "auto " + s->name + " = " +
                     (s->init ? gen_expr(s->init.get()) : "0") + ";");
            }
            break;
        }

        case Stmt::Return:
            if (s->kids.size() && s->kids[0])
                line("return " + gen_expr(s->kids[0].get()) + ";");
            else
                line("return;");
            break;

        case Stmt::Break:
            line("break;");
            break;
        case Stmt::Continue:
            line("continue;");
            break;

        case Stmt::ExprStmt:
            if (s->kids.size() && s->kids[0])
                line(gen_expr(s->kids[0].get()) + ";");
            break;

        default:
            // объявления функций/структур не инструкции — не сюда
            break;
    }
}

// ---------- верхний уровень ----------

bool top_name_of(const Stmt& s, std::string& name_out) {
    switch (s.kind) {
        case Stmt::FnDecl:
        case Stmt::Let:
        case Stmt::Const:
        case Stmt::StructDecl:
        case Stmt::EnumDecl:
            name_out = s.name;
            return true;
        default:
            return false;
    }
}

void CodeGen::declare_types(Stmt* root) {
    if (!root) return;
    for (const auto& s : root->body) {
        if (!s) continue;
        std::string dup;
        if (top_name_of(*s, dup) && !taken_.insert(dup).second) {
            saw_error_ = true;
            dg_.error("K3001", "имя '" + dup + "' объявлено в двух файлах проекта — "
                                           "для бэкенда это один и тот же C++ идентификатор",
                      s->span)
                .hint("переименуйте в одном из файлов или объедините их")
                .emit();
            continue;
        }
        if (s->kind == Stmt::StructDecl) {
            std::vector<std::string> fields;
            for (const auto& f : s->fields) fields.push_back(f.name);
            struct_fields_[s->name] = fields;
            line("struct " + s->name + " {");
            ind_++;
            for (const auto& f : s->fields) {
                line(type_str(f.type.get()) + " " + f.name + ";");
            }
            ind_--;
            line("};");
        } else if (s->kind == Stmt::EnumDecl) {
            enums_.insert(s->name);
            line("enum class " + s->name + " { " + join_comma(s->enum_items) + " };");
        }
    }
}

void CodeGen::define_globals(Stmt* root) {
    if (!root) return;
    for (const auto& s : root->body) {
        if (s && (s->kind == Stmt::Let || s->kind == Stmt::Const))
            gen_stmt(s.get());
    }
}

void CodeGen::define_fns(Stmt* root) {
    if (!root) return;
    for (const auto& s : root->body) {
        if (!s || s->kind != Stmt::FnDecl) continue;
        std::vector<std::string> params;
        for (const auto& p : s->params) {
            std::string pt = p.type ? type_str(p.type.get()) : "auto";
            params.push_back(pt + " " + p.name);
        }
        std::string ret = s->ret ? type_str(s->ret.get()) : "void";
        line("");
        line(ret + " " + s->name + "(" + join_comma(params) + ") {");
        ind_++;
        if (s->block) gen_body(s->block->body);
        ind_--;
        line("}");
    }
    // Node/isolate пока честно не поддерживаем.
    for (const auto& s : root->body) {
        if (!s) continue;
        if (s->kind == Stmt::NodeDecl || s->kind == Stmt::IsolateDecl)
            error_unsupported("K3001",
                              "'" + (s->kind == Stmt::NodeDecl ? std::string("node")
                                                               : std::string("isolate")) +
                                  "' в бэкенде пока не поддерживается — опишите "
                                  "обычные функции и app_entry",
                              s->span);
    }
}

void CodeGen::define_entry(Stmt* root) {
    if (!root) return;
    Stmt* entry = nullptr;
    for (const auto& s : root->body) {
        if (s && s->kind == Stmt::AppEntry) {
            entry = s.get();
            break;
        }
    }
    if (!entry) return;
    if (entry_done_) {
        saw_error_ = true;
        dg_.error("K3002", "в проекте несколько app_entry — в собранной программе "
                           "может быть только одна точка входа",
                  entry->span)
            .hint("оставьте app_entry в одном файле")
            .emit();
        return;
    }
    entry_done_ = true;
    line("");
    line("int main() {");
    ind_++;
    if (entry->block) gen_body(entry->block->body);
    line("return 0;");
    ind_--;
    line("}");
}

std::string stem_of(const std::string& path) {
    size_t a = path.find_last_of('/');
    std::string file = a == std::string::npos ? path : path.substr(a + 1);
    size_t d = file.find_last_of('.');
    return d == std::string::npos ? file : file.substr(0, d);
}

} // namespace kinii
