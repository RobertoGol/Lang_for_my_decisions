// Кодогенератор: AST -> C++20.
// Две простые гарантии: 1) мы выводим только то, что уже прошло семантику
// и оптимизацию; 2) если фича ещё не описана в бэкенде — честная ошибка K3001,
// а не «молча напечатаем что-то не то».

#include "kinii/codegen.h"
#include "kinii/ast.h"
#include "kinii/diagnostic.h"

#include <functional>
#include <iostream>
#include <sstream>

namespace kinii {

namespace {

// Замечание: прелюдия сгенерирована в CodeGen::begin() по флагам использований
// (scan_features) — см. там. Здесь её нет, потому что «компилятор не добавляет
// от себя ни байта»: без вызовов print/len не попадает ни функция, ни хедер.

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

// Точка-путь вида "console.print" или "sys.time.now"; пусто, если выражение
// не состоит из одних идентификаторов через точку.
std::string dotted_path(const Expr* e) {
    if (!e) return "";
    if (e->kind == Expr::Ident) return e->text;
    if (e->kind == Expr::Member && e->kids[0]) {
        std::string base = dotted_path(e->kids[0].get());
        if (base.empty()) return "";
        return base + "." + e->field;
    }
    return "";
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

// ---------- «компилятор не добавляет ни байта» ----------
// Прелюдия печатается ТОЛЬКО из того, что реально используется в проекте:
// runtime-функция без вызова не проходит, хедер без виновника не обходится.
// Мёртвый код не включён, скрытого рантайма нет.

// Скан использований: вызовы, строковые литералы, типы, узлы/изоляты.
//
// ВАЖНО: это « shotgun-сканер» — он отмечает флаг чуть щедрее, чем мог бы
// (например, len("abc") включает <string>), но никогда не обманывает наоборот:
// если флага нет — значит, использование точно не встретится в коде.

void CodeGen::scan_features(const Stmt* root) {
    if (!root) return;

    // Freestanding: всё, что требует кучу или потоки, — честная K3001.
    auto forbid_fs = [&](const std::string& what, const Span& sp) {
        saw_error_ = true;
        dg_.error("K3001",
                  "в freestanding-режиме (без CRT и libstdc++) недоступно: "
                      + what,
                  sp)
            .hint("там пока нет своей кучи — используйте числа, структуры "
                  "и массивы фикс. длины; обычный билд без --freestanding "
                  "поддерживает всё")
            .emit();
    };

    std::function<void(const TypeExpr*)> walk_type = [&](const TypeExpr* t) {
        if (!t) return;
        switch (t->kind) {
            case TypeExpr::Named:
                if (t->name == "string") {
                    need_string_ = true;
                    if (freestanding_)
                        forbid_fs("строки (string)", t->span);
                }
                break;
            case TypeExpr::List:
                need_vector_ = true;
                if (freestanding_)
                    forbid_fs("динамические списки (list<T>)", t->span);
                walk_type(t->elem.get());
                break;
            case TypeExpr::Map:
                need_map_ = true;
                if (freestanding_)
                    forbid_fs("словари (map<K, V>) — нужна своя куча", t->span);
                walk_type(t->elem.get());
                walk_type(t->val.get());
                break;
            case TypeExpr::Array:
                need_array_ = true;
                walk_type(t->elem.get());
                break;
            case TypeExpr::Cell:
                walk_type(t->elem.get());
                break;
            default: break;
        }
    };

    std::function<void(const Expr*)> walk_expr = [&](const Expr* e) {
        if (!e) return;
        if (e->kind == Expr::Str) {
            need_string_ = true;
            // литеральная строка честно тоже в запрет: тип выводится в string
            // (std::string тянет кучу CRT). Печать чисел ограничена честно.
            if (freestanding_)
                forbid_fs("строка (литерал и переменная string) — своя куча "
                          "в CtOs появится отдельно", e->span);
        }
        if (e->kind == Expr::ListLit && freestanding_) {
            forbid_fs("список-литерал (list<T>) — своя куча в CtOs появится "
                      "отдельно", e->span);
        }
        if (e->kind == Expr::MapLit && freestanding_) {
            forbid_fs("словарь-литерал (map<K, V>) — своя куча в CtOs появится "
                      "отдельно", e->span);
        }
        if (e->kind == Expr::Call && !e->kids.empty() &&
            e->kids[0]) {
            std::string callee_path =
                dotted_path(e->kids[0].get());
            if (e->kids[0]->kind == Expr::Ident)
                callee_path = e->kids[0]->text;
            else if (e->kids[0]->kind == Expr::Member) {
                std::string base = dotted_path(e->kids[0]->kids[0].get());
                if (!base.empty())
                    callee_path = base + "." + e->kids[0]->field;
            }
            if (callee_path == "print" || callee_path == "console.print")
                need_print_ = true;
            else if (callee_path == "len")
                need_len_ = true;
            else if (callee_path == "console.read_input" ||
                     callee_path == "console.read_line") {
                need_read_ = true;
                if (freestanding_)
                    forbid_fs("чтение ввода (console.read_*) — без libc; "
                              "сделаем после своей кучи", e->span);
            } else if (callee_path == "sys.exit")
                need_exit_ = true;
            else if (callee_path == "sys.time.now")
                need_time_ = true;
            else if (callee_path == "sys.rnd.next")
                need_rnd_ = true;
        }
        for (const auto& k : e->kids) walk_expr(k.get());
    };

    std::function<void(const Stmt*)> walk_stmt = [&](const Stmt* s) {
        if (!s) return;
        walk_type(s->type.get());
        walk_type(s->ret.get());
        for (const auto& p : s->params) walk_type(p.type.get());
        for (const auto& f : s->fields) walk_type(f.type.get());
        walk_expr(s->init.get());
        for (const auto& k : s->kids) walk_expr(k.get());
        for (const auto& b : s->body) walk_stmt(b.get());
        for (const auto& b : s->else_body) walk_stmt(b.get());
        for (const auto& c : s->cases) walk_expr(c.value.get());
        if (s->block) walk_stmt(s->block.get());
        if (s->kind == Stmt::NodeDecl || s->kind == Stmt::IsolateDecl) {
            need_thread_ = true;
            if (freestanding_)
                forbid_fs("node/isolate (потоки) — без libc; сделаем после "
                          "своей кучи", s->span);
        }
    };
    walk_stmt(root);
}

void CodeGen::begin() {
    os_ << "// Сгенерировано kinic из KINII v3 — является выходом компилятора.\n";
    os_ << "// Правки вручную потеряются: следующий запуск перезапишет файл.\n";
    os_ << "// Прелюдия подключает только то, что реально использовано: мёртвый код\n";
    os_ << "// и неиспользованные хедеры в бинарник не попадают.\n";
    if (freestanding_) {
        os_ << R"RT(#include <cstdint>

// freestanding: только kernel32. Без CRT, без libstdc++, без <type_traits> —
// печать честно на перегрузках, знак числа — runtime-проверкой (для
// беззнаковых типов ветка мёртвая и выкидывается компилятором).
// Зависимость: kernel32.dll (есть всегда).
extern "C" {
void ExitProcess(uint32_t code);
void* GetStdHandle(int32_t which);
int WriteFile(void* handle, const void* data, uint32_t bytes,
              uint32_t* written, void* overlapped);
void GetSystemTimeAsFileTime(uint64_t* out);
uint64_t GetTickCount64(void);
}
extern "C" int main(void);
// gcc в MinGW вставляет в main вызов __main (инициализация конструкторов);
// в freestanding их нет — честная пустышка.
extern "C" void __main() {}

extern "C" void _start() {
    ExitProcess((uint32_t)main());
}

namespace kn {
inline void* std_out() { return GetStdHandle(-11 /*STD_OUTPUT_HANDLE*/); }

inline void kn_write(const char* s, uint32_t n) {
    uint32_t w = 0;
    WriteFile(std_out(), s, n, &w, nullptr);
}
inline void kn_lit(const char* s) {
    uint32_t n = 0;
    while (s[n]) ++n;
    kn_write(s, n);
}
template <class T>
inline void kn_num(T v) {
    char tmp[24];
    int i = 0;
    if (v < 0) {                       // честно: беззнаковым не зайдёт
        tmp[i++] = '-';
        uint64_t u = (uint64_t)(-(v + 1)) + 1;   // безопасно для самого младшего
        do { tmp[i++] = char('0' + u % 10); u /= 10; } while (u);
    } else {
        uint64_t u = (uint64_t)v;
        do { tmp[i++] = char('0' + u % 10); u /= 10; } while (u);
    }
    while (i) kn_write(&tmp[--i], 1);
}
// одно значение — один вызов: перегрузки вместо constexpr-if
inline void kn_one(const char* v) { kn_lit(v); }
inline void kn_one(bool v) { kn_write(v ? "1" : "0", 1); }
inline void kn_one(unsigned char v) { kn_num<int>((int)v); }
inline void kn_one(signed char v) { kn_num<int>((int)v); }
inline void kn_one(short v) { kn_num(v); }
inline void kn_one(unsigned short v) { kn_num(v); }
inline void kn_one(int v) { kn_num(v); }
inline void kn_one(unsigned int v) { kn_num(v); }
inline void kn_one(long v) { kn_num(v); }
inline void kn_one(unsigned long v) { kn_num(v); }
inline void kn_one(long long v) { kn_num(v); }
inline void kn_one(unsigned long long v) { kn_num(v); }
// print печатает аргументы подряд и переводит строку (как всегда).
template <class... A>
void print(const A&... a) {
    ((kn_one(a), 0), ...);
    kn_write("\n", 1);
}
)RT";
        // freestanding: все замены — только kernel32, без libc/кучи.
        if (need_exit_) {
            os_ << R"RT([[noreturn]] inline void sys_exit(uint64_t code) {
    ExitProcess((uint32_t)code);
}
)RT";
        }
        if (need_time_) {
            os_ << R"RT(inline uint64_t time_now_ms() {
    // FILETIME — 100-нс тики с 1601-01-01 UTC; сдвиг эпохи вычтен честно.
    uint64_t ft = 0;
    GetSystemTimeAsFileTime(&ft);
    return (ft - 116444736000000000ULL) / 10000;
}
)RT";
        }
        if (need_rnd_) {
            os_ << R"RT(inline uint64_t rnd_next(uint64_t limit) {
    // xorshift64 (Г. Марсалья): без libc и без <random>. Нулевое состояние
    // честно исключено (нули зациклены): сид — GetTickCount64, подмешанный
    // константой золотого сечения, плюс |1.
    static uint64_t st = 0;
    if (st == 0)
        st = ((GetTickCount64() ^ 0x9E3779B97F4A7C15ULL) | 0xB5297A4DC58B56F7ULL);
    st ^= st >> 12; st ^= st << 25; st ^= st >> 27;
    // честный равномерный отдача: старшие биты 128-битного произведения,
    // смещения модуля нет.
    return (uint64_t)(((unsigned __int128)st * limit) >> 64);
}
)RT";
        }
        os_ << "} // namespace kn\n";
        return;
    }
    os_ << "#include <cstdint>\n";
    if (need_string_) os_ << "#include <string>\n";
    if (need_vector_) os_ << "#include <vector>\n";
    if (need_array_) os_ << "#include <array>\n";
    if (need_map_) os_ << "#include <unordered_map>\n";
    if (need_thread_) os_ << "#include <thread>\n";
    if (need_exit_) os_ << "#include <cstdlib>\n";
    // freestanding: <chrono>/<random> тянут libc и кучу — честные замены на kernel32.
    if (need_time_ && !freestanding_) os_ << "#include <chrono>\n";
    if (need_rnd_ && !freestanding_) os_ << "#include <random>\n";

    // Честный фикс B-04: инклуды прелюдии обязаны лежать ДО namespace kn,
    // иначе они становятся kn::std и сборка держится на везении include-guard'ов
    // (проект с print/len/read_* без string/list/map честно не собирался).
    if (need_print_) os_ << "#include <type_traits>\n";
    if (need_print_ || need_read_) os_ << "#include <cstdio>\n";

    std::string rt = "namespace kn {\n";
    if (need_print_) {
        rt += R"RT(template <class T>
std::string text_of(const T& v) {
    if constexpr (std::is_same_v<std::decay_t<T>, const char*>)
        return std::string(v);
    else if constexpr (std::is_same_v<std::decay_t<T>, bool>)
        return v ? "1" : "0";
    else if constexpr (std::is_integral_v<T> && sizeof(T) == 1)
        return std::to_string((int)v);
    else if constexpr (std::is_integral_v<T>)
        return std::to_string(v);
    else
        return v;
}
// print печатает аргументы подряд и переводит строку (как всегда).
template <class... A>
void print(const A&... a) {
    std::string out;
    ((out += text_of(a)), ...);
    out += '\n';
    fputs(out.c_str(), stdout);
}
)RT";
    }
    if (need_len_) {
        rt += R"RT(template <class C>
auto len(const C& c) -> decltype(c.size()) {
    return c.size();
}
)RT";
    }
    if (need_read_) {
        rt += R"RT(inline std::string read_line(const std::string& prompt = "") {
    if (!prompt.empty()) fputs(prompt.c_str(), stdout);
    std::string s;
    int c;
    while ((c = fgetc(stdin)) != EOF && c != '\n')
        s.push_back((char)c);
    return s;
}
)RT";
    }
    if (need_exit_) {
        if (freestanding_) {
            rt += R"RT([[noreturn]] inline void sys_exit(uint64_t code) {
    // freestanding: без libc честный выход — ExitProcess (kernel32).
    ExitProcess((uint32_t)code);
}
)RT";
        } else {
            rt += R"RT([[noreturn]] inline void sys_exit(uint64_t code) {
    std::exit((int)code);
}
)RT";
        }
    }
    if (need_time_) {
        if (freestanding_) {
            rt += R"RT(inline uint64_t time_now_ms() {
    // freestanding: FILETIME — 100-нансекундные тики с 1601-01-01 UTC.
    // Сдвиг эпохи честно вычтен: получаем миллисекунды с 1970-01-01.
    uint64_t ft = 0;
    GetSystemTimeAsFileTime(&ft);
    return (ft - 116444736000000000ULL) / 10000;
}
)RT";
        } else {
            rt += R"RT(inline uint64_t time_now_ms() {
    using namespace std::chrono;
    return (uint64_t)duration_cast<milliseconds>(
        system_clock::now().time_since_epoch()).count();
}
)RT";
        }
    }
    if (need_rnd_) {
        if (freestanding_) {
            rt += R"RT(inline uint64_t rnd_next(uint64_t limit) {
    // freestanding: xorshift64 (Г. Марсалья) — без libc и без <random>.
    // Нулевое состояние честно исключено (xorshift на нуле зациклен):
    // сид — GetTickCount64, смешанный константами золотого сечения.
    static uint64_t st = 0;
    if (st == 0)
        st = ((GetTickCount64() ^ 0x9E3779B97F4A7C15ULL)
              | 0xB5297A4DC58B56F7ULL);
    st ^= st >> 12; st ^= st << 25; st ^= st >> 27;
    // честный равномерный отдача: старшие биты 128-битного произведения
    // (умножение со взятием верхней половины — смещения нет).
    return (uint64_t)(((unsigned __int128)st * limit) >> 64);
}
)RT";
        } else {
            rt += R"RT(inline uint64_t rnd_next(uint64_t limit) {
    if (limit == 0) return 0;
    static std::mt19937_64 rng{ std::random_device{}() };
    std::uniform_int_distribution<uint64_t> d(0, limit - 1);
    return d(rng);
}
)RT";
        }
    }
    rt += "} // namespace kn\n";
    if (rt != "namespace kn {\n} // namespace kn\n") os_ << rt << "\n";

    if (need_string_) {
        // «строка + число» — это склейка, а не арифметика указателей.
        os_ << R"RT(template <class T>
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
    }
}

std::string CodeGen::type_str(const TypeExpr* t) {
    if (!t) return "auto";
    switch (t->kind) {
        case TypeExpr::Named:
            if (nodes_.count(t->name)) {
                saw_error_ = true;
                dg_.error("K3001",
                          "узел '" + t->name + "' не является типом — это единица "
                          "исполнения: у него свой поток",
                          t->span)
                    .hint("состояние узла храните внутри struct, узел читает/пишет её")
                    .emit();
                return "void";
            }
            return builtin_type(t->name);
        case TypeExpr::Cell:
            return type_str(t->elem.get()); // модификатор cell на выход не влияет
        case TypeExpr::List:
            return "std::vector<" + type_str(t->elem.get()) + ">";
        case TypeExpr::Map:
            return "std::unordered_map<" + type_str(t->elem.get()) + ", " +
                   type_str(t->val.get()) + ">";
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
                // Методы словаря: m.contains(k), m.remove(k), m.keys(),
                // m.values(), m.size() — типы K/V известны из объявления.
                const Expr* mbase = callee->kids[0].get();
                if (mbase && mbase->kind == Expr::Ident) {
                    auto mv = map_vars_.find(mbase->text);
                    if (mv != map_vars_.end()) {
                        const std::string& var = mv->first;
                        size_t sep = mv->second.find('\x1F');
                        std::string kt = mv->second.substr(0, sep);
                        std::string vt = mv->second.substr(sep + 1);
                        const std::string& meth = callee->field;
                        auto wrong_args = [&](const std::string& want) {
                            saw_error_ = true;
                            dg_.error("K2006",
                                      "метод '" + var + "." + meth + "' принимает " +
                                          want,
                                      callee->span)
                                .hint("у map<K, V> есть: contains(k), remove(k), "
                                      "keys(), values(), size()")
                                .emit();
                        };
                        std::string arg =
                            e->kids.size() > 1 ? gen_expr(e->kids[1].get()) : "";
                        if (meth == "contains" || meth == "remove") {
                            if (e->kids.size() != 2) {
                                wrong_args(meth == "contains"
                                               ? "1 аргумент (ключ)"
                                               : "1 аргумент (ключ)");
                                return "0";
                            }
                            if (meth == "contains")
                                return "(" + var + ".count(" + arg + ") > 0)";
                            return var + ".erase(" + arg + ")";
                        }
                        if (meth == "size") {
                            if (e->kids.size() != 1) {
                                wrong_args("0 аргументов");
                                return "0";
                            }
                            return var + ".size()";
                        }
                        if (meth == "keys") {
                            if (e->kids.size() != 1) {
                                wrong_args("0 аргументов");
                                return "0";
                            }
                            // list<K>: все ключи словаря в порядке хранения
                            return "[&]{ std::vector<" + kt + "> r; "
                                   "for (const auto& kv : " + var +
                                   ") r.push_back(kv.first); return r; }()";
                        }
                        if (meth == "values") {
                            if (e->kids.size() != 1) {
                                wrong_args("0 аргументов");
                                return "0";
                            }
                            // list<V>: все значения словаря
                            return "[&]{ std::vector<" + vt + "> r; "
                                   "for (const auto& kv : " + var +
                                   ") r.push_back(kv.second); return r; }()";
                        }
                        // неизвестный метод словаря — честная ошибка
                        saw_error_ = true;
                        dg_.error("K3001",
                                  "у словаря нет метода '" + meth + "'",
                                  callee->span)
                            .hint("у map<K, V> есть: contains(k), remove(k), "
                                  "keys(), values(), size(); размер ещё даёт len(m)")
                            .emit();
                        return "0";
                    }
                }

                // Системные вызовы с точкой: console.print(...), sys.exit(...) и т.п.
                std::string path = dotted_path(callee);
                std::vector<std::string> args;
                for (size_t i = 1; i < e->kids.size(); ++i)
                    args.push_back(gen_expr(e->kids[i].get()));

                if (path == "console.print")
                    return "kn::print(" + join_comma(args) + ")";
                if (path == "console.read_input" || path == "console.read_line")
                    return "kn::read_line(" + join_comma(args) + ")";
                if (path == "sys.exit")
                    return "kn::sys_exit(" + join_comma(args) + ")";
                if (path == "sys.time.now")
                    return "kn::time_now_ms(" + join_comma(args) + ")";
                if (path == "sys.rnd.next")
                    return "kn::rnd_next(" + join_comma(args) + ")";

                if (!path.empty()) {
                    saw_error_ = true;
                    dg_.error("K3001",
                              "системный вызов '" + path + "' не входит в бэкенд",
                              callee->span)
                        .hint("поддерживаются: console.print, console.read_input, "
                              "console.read_line, sys.exit, sys.time.now, sys.rnd.next")
                        .hint("что-то другое из sys.* — опишите обычной функцией")
                        .emit();
                } else {
                    // Честный страховочный фильтр B-07: сюда доходит метод на
                    // результате вызова/индекса — раньше это молча давало 0.
                    saw_error_ = true;
                    dg_.error("K3004",
                              "метод на результате вызова/индекса не поддержан "
                              "бэкендом",
                              callee->span)
                        .hint("сначала сохраните значение в переменную")
                        .emit();
                }
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
            // Запись перебора словаря: pair.key -> .first, pair.value -> .second
            if (e->kids[0] && e->kids[0]->kind == Expr::Ident) {
                auto ev = entry_vars_.find(e->kids[0]->text);
                if (ev != entry_vars_.end()) {
                    if (e->field == "key")
                        return gen_expr(e->kids[0].get()) + ".first";
                    if (e->field == "value")
                        return gen_expr(e->kids[0].get()) + ".second";
                    saw_error_ = true;
                    dg_.error("K3001",
                              "у записи словаря нет поля '" + e->field + "'",
                              e->span)
                        .hint("запись даёт ровно два поля: .key (ключ) и "
                              ".value (значение)")
                        .emit();
                    return "0";
                }
            }
            // свойство словаря: m.size — число пар (без скобок, как у человека)
            if (e->kids[0] && e->kids[0]->kind == Expr::Ident) {
                auto mv = map_vars_.find(e->kids[0]->text);
                if (mv != map_vars_.end()) {
                    if (e->field == "size") return mv->first + ".size()";
                    saw_error_ = true;
                    dg_.error("K3001",
                              "у словаря нет свойства '" + e->field + "'",
                              e->span)
                        .hint("единственный доступный размер: " + mv->first +
                              ".size (или len(" + mv->first + "))")
                        .emit();
                    return "0";
                }
            }
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

        case Expr::MapLit: {
            // пары ключ/значение: замыкающий инициализатор unordered_map
            std::vector<std::string> pairs;
            for (size_t i = 0; i + 1 < e->kids.size(); i += 2) {
                const Expr* k = e->kids[i].get();
                const Expr* v = e->kids[i + 1].get();
                if (!k || !v) continue;
                pairs.push_back("{" + gen_expr(k) + ", " + gen_expr(v) + "}");
            }
            return "{" + join_comma(pairs) + "}";
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
            // Перебор словаря: элементы — пары, читаемые как .key / .value
            const bool over_map = !s->kids.empty() &&
                                  s->kids[0]->kind == Expr::Ident &&
                                  map_vars_.count(s->kids[0]->text) > 0;
            if (over_map) entry_vars_.insert(s->name);
            line("for (auto& " + s->name + " : " + src + ") {");
            ind_++;
            if (!s->body.empty() && s->body[0] && s->body[0]->kind == Stmt::Block)
                gen_body(s->body[0]->body);
            else
                gen_body(s->body);
            ind_--;
            if (over_map) entry_vars_.erase(s->name);
            line("}");
            break;
        }

        case Stmt::Decide: {
            // Ветвление выводим как цепочку if/else, а не switch:
            //   1) работает и по строкам («weight "SQL" => { ... }»);
            //   2) break внутри ветки честно выходит из ближайшего цикла,
            //      как его читает человек — decide это ветвление, а не цикл.
            // Тема вычисляется один раз: side effects не дублируются.
            if (s->cases.empty()) break;
            std::string subj = s->kids.size() ? gen_expr(s->kids[0].get()) : "0";
            line("{");
            ind_++;
            line("auto&& kn_s = (" + subj + ");");
            // сначала все weight-ветки в порядке написания, else — всегда последней
            const Stmt::Case* else_case = nullptr;
            bool chain_open = false;
            for (const auto& c : s->cases) {
                if (!c.value) { else_case = &c; continue; }
                line((chain_open ? "else if (kn_s == " : "if (kn_s == ") +
                     gen_expr(c.value.get()) + ") {");
                chain_open = true;
                ind_++;
                gen_body(c.body);
                ind_--;
                line("}");
            }
            if (else_case) {
                line(chain_open ? "else {" : "if (true) {");
                ind_++;
                gen_body(else_case->body);
                ind_--;
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
                if (s->type->kind == TypeExpr::Map)
                    map_vars_[s->name] = type_str(s->type->elem.get()) + '\x1F' +
                                         type_str(s->type->val.get());
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
            } else if (s->init && s->init->kind == Expr::MapLit &&
                       s->init->kids.size() >= 2) {
                // словарь без типа: выводим K и V из первой пары
                auto kind_cxx = [this](const Expr* x) -> std::string {
                    if (!x) return "int64_t";
                    if (x->kind == Expr::Str) return "std::string";
                    if (x->kind == Expr::Bool) return "bool";
                    if (x->kind == Expr::Num) return "int64_t";
                    return "decltype(" + gen_expr(x) + ")";
                };
                line(kw + "std::unordered_map<" +
                     kind_cxx(s->init->kids[0].get()) + ", " +
                     kind_cxx(s->init->kids[1].get()) + "> " + s->name + " = " +
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
        case Stmt::NodeDecl:
        case Stmt::IsolateDecl:
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
        if (s->kind == Stmt::NodeDecl || s->kind == Stmt::IsolateDecl) {
            // узел/изолят — не структура и не перечисление: его имя не тип
            nodes_.insert(dup);
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

// Печать одной функции верхнего уровня.
void CodeGen::emit_fn(Stmt* s) {
    std::vector<std::string> params;
    for (const auto& p : s->params) {
        std::string pt = p.type ? type_str(p.type.get()) : "auto";
        params.push_back(pt + " " + p.name);
        if (p.type && p.type->kind == TypeExpr::Map)
            map_vars_[p.name] = type_str(p.type->elem.get()) + '\x1F' +
                                type_str(p.type->val.get());
    }
    std::string ret = s->ret ? type_str(s->ret.get()) : "void";
    line("");
    line(ret + " " + s->name + "(" + join_comma(params) + ") {");
    ind_++;
    if (s->block) gen_body(s->block->body);
    ind_--;
    line("}");
}

void CodeGen::define_fns(Stmt* root) {
    if (!root) return;
    for (const auto& s : root->body) {
        if (!s) continue;

        if (s->kind == Stmt::FnDecl) {
            emit_fn(s.get());
            continue;
        }

        // node/isolate: Linux-модель — модуль как задача.
        //   * его функции поднимаются в проект (видны всем файлам);
        //   * его инструкции образуют тело задачи, которая стартует
        //     в начале main и всегда join-ится перед его выходом —
        //     программа не завершается раньше своих задач («systemd-юнит»).
        if (s->kind == Stmt::NodeDecl || s->kind == Stmt::IsolateDecl) {
            if (!s->block) continue;

            // функции узла — обычные функции проекта (межфайловые дубли ловим)
            bool has_fn = false;
            for (const auto& inner : s->block->body) {
                if (!inner || inner->kind != Stmt::FnDecl) continue;
                has_fn = true;
                if (!taken_.insert(inner->name).second) {
                    saw_error_ = true;
                    dg_.error("K3001",
                              "имя '" + inner->name + "' объявлено в двух местах "
                              "проекта — для бэкенда это один и тот же C++ "
                              "идентификатор",
                              inner->span)
                        .hint("переименуйте функцию в узле '" + s->name + "'")
                        .emit();
                    continue;
                }
                emit_fn(inner.get());
            }

            // инструкции тела узла -> отдельная задача-функция
            std::vector<Stmt*> plain;
            for (const auto& inner : s->block->body)
                if (inner && inner->kind != Stmt::FnDecl) plain.push_back(inner.get());
            if (plain.empty() && !has_fn) continue; // пустой узел — не задача

            std::string fname =
                "kn_iso_body_" + std::to_string(iso_jobs_.size());
            line("");
            line("void " + fname + "() {");
            ind_++;
            for (Stmt* st : plain) gen_stmt(st);
            ind_--;
            line("}");
            iso_jobs_.push_back(fname);
            (void)has_fn;
        }
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
    // Задачи узлов стартуют вместе с приложением — как systemd-юниты.
    for (const auto& job : iso_jobs_) {
        line("");
        line("std::thread " + job + "_t(" + job + ");");
    }
    line("");
    line("int main() {");
    ind_++;
    if (entry->block) gen_body(entry->block->body);
    // приложение не завершается раньше своих задач
    for (const auto& job : iso_jobs_) {
        line(job + "_t.join();");
    }
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
