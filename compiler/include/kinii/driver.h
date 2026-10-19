#pragma once
// Драйвер компиляции: проходит цепочки import между файлами.
// Двухфазная работа:
//   1) compile(path) — загрузить файл и его импорты рекурсивно, построить AST
//      (без проверок имён: таблица символов ещё не полна);
//   2) finalize() — семантика всех файлов, зависимости в первую очередь:
//      так имена из импортированных файлов уже известны проверяющим.

#include "kinii/common.h"
#include "kinii/diagnostic.h"
#include "kinii/parser.h"
#include "kinii/sema.h"

#include <map>
#include <set>
#include <string>
#include <vector>

namespace kinii {

class Driver {
public:
    Driver(DiagnosticEngine& dg, SemaResult& res) : dg_(dg), res_(res) {}

    // Фаза 1: загрузка графа файлов. Возвращает FileId или kNoFile.
    FileId compile(const std::string& path, const Span& at_import);

    // Фаза 2: семантика загруженных файлов (зависимости раньше зависящих).
    void finalize();

    // AST загруженного файла (или null). Используется оптимизатором/кодогенерацией.
    Stmt* ast_of(FileId fid);

    // Порядок открытия файлов (младший FileId — зависимости грузятся первыми).
    const std::vector<FileId>& order() const { return order_; }

    // Содержимое из редактора (didOpen/didChange LSP) приоритетно перед диском.
    void set_source_override(const std::string& path, const std::string& src) {
        overrides_[normalize(path)] = src;
    }

private:
    static std::string normalize(std::string p);

    DiagnosticEngine& dg_;
    SemaResult& res_;
    std::map<std::string, FileId> loaded_;
    std::set<std::string> loading_;
    std::vector<FileId> order_;
    std::map<std::string, std::string> overrides_;

    struct Unit {
        FileId fid = kNoFile;
        StmtP ast;
    };
    std::map<FileId, Unit> units_;
};

} // namespace kinii
