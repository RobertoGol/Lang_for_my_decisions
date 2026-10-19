#pragma once
// Таблица символов и результаты семантического анализа.

#include "kinii/ast.h"
#include "kinii/common.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace kinii {

class DiagnosticEngine;

// Один символ: переменная, функция, структура, перечисление, поле.
struct Symbol {
    enum Kind { Var, Const, Fn, Struct, Enum, Field, Param, Builtin };
    Kind kind = Var;

    std::string name;
    std::string type_text;   // человекочитаемый тип ("u32", "list<string>", "cell:raw")
    uint32_t type_size = 0;  // если известен размер в байтах (для «компактности»)
    bool is_const = false;
    bool is_pub = false;
    bool used = false;       // пригодится оптимизатору (мёртвый код)

    // Где объявлен (для ссылок между файлами: «объявлен здесь»)
    FileId file = kNoFile;
    Span span;

    // Для Fn
    int param_count = -1;
};

// Область видимости: стек вложенных блоков.
struct Scope {
    std::map<std::string, Symbol> syms;
    Scope* parent = nullptr; // сырой указатель — стеком владеет Sema (vector)

    Symbol* find(const std::string& name);
    const Symbol* find(const std::string& name) const;
};

// Результат семантики одного файла.
struct FileSema {
    FileId file = kNoFile;
    std::string path;                        // путь файла (для сообщений)
    std::vector<std::string> imports;        // пути импорта как в коде
    std::vector<FileId> imported_ids;        // FileId реально подключённых файлов
    std::map<std::string, Symbol> top_decls; // всё верхнеуровневое этого файла
};

// Состояние семантики всего проекта.
struct SemaResult {
    std::vector<FileSema> files;
    std::map<std::string, Symbol> global; // все верхнеуровневые объявления проекта
    bool project_wide = false; // «как в C#»: выбранные файлы видят друг друга сами,
                               // import не обязателен
    size_t unknown_symbols = 0;
};

// Полный семантический проход по одному файлу (диагностики уходят в dg).
void run_sema(FileId fid, const StmtP& ast, DiagnosticEngine& dg, SemaResult& res);

} // namespace kinii
