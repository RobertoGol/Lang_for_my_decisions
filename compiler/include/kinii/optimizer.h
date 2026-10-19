#pragma once
// Оптимизатор KINII.
// Идея: компилятор сам убирает лишнее, а программист видит отчёт
// «что я сделал и почему». Ни одна оптимизация не меняет смысла программы.
//
// Что умеет:
//   - свёртка констант («2 * 3» -> «6», сравнения, &&/|| с константами);
//   - if/while/decide/тернарник с константным условием -> остаются только живые ветки;
//   - мёртвый код: переменные, которые не читаются; выражения без эффекта;
//     код после return/break/continue.

#include "kinii/common.h"

#include <set>
#include <string>
#include <vector>

namespace kinii {

struct Stmt; // ast.h

enum class OptKind { Fold, Dead, Branch, Advice };

struct OptAction {
    std::string file;
    size_t line = 0;
    std::string text;
    OptKind kind;
};

struct OptReport {
    std::vector<OptAction> actions;
    size_t folded = 0;    // свёрток констант
    size_t dead = 0;      // удалено штук мёртвого кода
    size_t branches = 0;  // удалено ветвей
    size_t nodes_before = 0; // суммарный размер AST до оптимизации
    size_t nodes_after = 0;  // суммарный размер AST после
};

// Проходит по AST одного файла, применяет оптимизации на месте и наполняет отчёт.
void optimize_file(Stmt* root, const std::string& path, OptReport& rep);

// Собирает все прочитанные имена в файле (для «этот импорт не используется»).
void collect_reads(Stmt* root, std::set<std::string>& out);

// Число узлов (инструкции + выражения) — метрика «сколько места занимает код».
size_t count_nodes(const Stmt* root);

} // namespace kinii
