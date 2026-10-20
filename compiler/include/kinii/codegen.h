#pragma once
// Кодогенерация KINII.
// Уровень: AST (уже проверенный семантикой и оптимизированный) -> чистый C++20.
// Полученный файл собирается внешним компилятором (g++ -O2), поэтому вывод
// компактен, а программист при желании может открыть сгенерированный .cpp и
// увидеть знакомые конструкции.
//
// Порядок генерации (важно для C++):
//   1) преамбула со встроенным рантаймом (print/len);
//   2) все структуры и перечисления проекта;
//   3) верхнеуровневые let/const;
//   4) все функции;
//   5) app_entry -> int main() (ровно один).

#include "kinii/common.h"

#include <iosfwd>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace kinii {

struct Stmt;
struct Expr;
struct TypeExpr;
class DiagnosticEngine;

class CodeGen {
public:
    CodeGen(DiagnosticEngine& dg, std::ostream& os);

    // Прецедент: include-ы + минимальный рантайм.
    void begin();

    // Шаг 1: структуры/перечисления файла; шаг 2: константы; шаг 3: функции.
    void declare_types(Stmt* root);
    void define_globals(Stmt* root);
    void define_fns(Stmt* root);

    // app_entry -> int main (ровно один на проект).
    void define_entry(Stmt* root);

    bool ok() const { return !saw_error_; }

private:
    DiagnosticEngine& dg_;
    std::ostream& os_;
    int ind_ = 0;                 // текущий отступ
    std::set<std::string> enums_; // имена перечислений проекта
    std::map<std::string, std::vector<std::string>> struct_fields_; // поля в порядке объявления
    std::set<std::string> taken_; // имена верхнего уровня (межфайловые дубли)
    bool saw_error_ = false;
    bool entry_done_ = false;

    void error_unsupported(const char* code, const std::string& what, const Span& sp);
    void out_indent();
    void line(const std::string& text);

    std::string type_str(const TypeExpr* t);
    std::string gen_expr(const Expr* e);
    void gen_stmt(Stmt* s);
    void gen_body(const std::vector<std::unique_ptr<Stmt>>& body);

    // Печать одной функции верхнего уровня (в т.ч. функций из node/isolate).
    void emit_fn(Stmt* s);

    std::set<std::string> nodes_;         // имена node/isolate (не типы!)
    std::vector<std::string> iso_jobs_;   // kn_iso_body_N — потоки стартуют из main

    // Переменные с известным типом map<K, V>: имя -> «K\x1FV» (C++-типы).
    // Нужны бэкенду для честных методов словаря (keys()/values() и т.д.).
    std::map<std::string, std::string> map_vars_;
};

// Готовит имя exe по умолчанию (стебель входного файла) — helper для CLI.
std::string stem_of(const std::string& path);

} // namespace kinii
