#pragma once
// AST (абстрактное синтаксическое дерево) KINII.
// Парсер строит дерево, семантика и оптимизатор ходят по нему.

#include "kinii/common.h"
#include <memory>
#include <string>
#include <vector>

namespace kinii {

// ---------- Тип ----------
struct TypeExpr {
    enum Kind { Named, Cell, List, Array, Map };
    Kind kind = Named;
    std::string name;                // Named: "u32", "Point"; Cell: модификатор ("" = vocal)
    std::unique_ptr<TypeExpr> elem;  // List<T>: T; Map<K,V>: K
    std::unique_ptr<TypeExpr> val;   // Map<K,V>: V
    uint32_t array_len = 0;          // Array: длина
    Span span;
};

using TypeP = std::unique_ptr<TypeExpr>;

// ---------- Выражение ----------
// Смысл kids зависит от kind (см. parser.cpp):
//   Binary/Unary/Assign: оператор в text; Assign/Unary: [x]; Binary: [lhs, rhs]
//   Ternary: [cond, then, else]
//   Call: [callee, arg1, arg2...]
//   Member: [obj], поле в field
//   Index: [obj, index]
//   ListLit: элементы
//   MapLit: пары [ключ, значение, ключ, значение, ...]
//   StructLit: имя типа в field, имена полей в fields, значения в kids
//   Comprehension: [элемент, источник, условие?], переменная в text
struct Expr {
    enum Kind {
        Num, Str, Bool, Ident,
        Binary, Unary, Ternary, Assign,
        Call, Member, Index,
        ListLit, MapLit, StructLit, Comprehension,
    };
    Kind kind;
    Span span;
    std::string text;
    std::string field;
    std::vector<std::unique_ptr<Expr>> kids;
    std::vector<std::string> fields;

    explicit Expr(Kind k) : kind(k) {}
};

using ExprP = std::unique_ptr<Expr>;

// ---------- Параметр функции ----------
struct Param {
    std::string name;
    TypeP type;
    Span span;
};

// ---------- Поле структуры ----------
struct FieldDecl {
    std::string name;
    TypeP type;
    Span span;
};

// ---------- Инструкция / объявление ----------
struct Stmt {
    enum Kind {
        Let, Const,
        If, While, Loop, ForIn, Decide,
        Return, Break, Continue,
        ExprStmt, Block, AppEntry,
        FnDecl, StructDecl, EnumDecl, NodeDecl, IsolateDecl, Import,
    };
    Kind kind;
    Span span;

    std::string name;              // Let/Const/объявления — имя; Import — путь
    Span name_span;                // спан только имени (для меток «объявлен здесь»)
    TypeP type;                    // Let: тип (null = вывести сам)
    ExprP init;                    // Let/Const — начальное значение

    std::vector<Param> params;     // FnDecl
    TypeP ret;                     // FnDecl
    bool is_pub = false, is_priv = false, is_async = false, is_entry = false, is_handler = false;

    std::vector<ExprP> kids;       // If: [cond]; While: [cond]; ForIn: [источник];
                                   // Return: [значение?]; ExprStmt: [выражение]
    std::vector<std::unique_ptr<Stmt>> body;      // Block, ветки if/for/loop, члены node/isolate
    std::vector<std::unique_ptr<Stmt>> else_body; // If: ветка else
    std::unique_ptr<Stmt> block;                  // FnDecl/NodeDecl/IsolateDecl/AppEntry: тело

    struct Case {
        ExprP value;               // null для else
        std::vector<std::unique_ptr<Stmt>> body;
        Span span;
    };
    std::vector<Case> cases;       // Decide

    std::vector<FieldDecl> fields;      // StructDecl
    std::vector<std::string> enum_items; // EnumDecl

    explicit Stmt(Kind k) : kind(k) {}
};

using StmtP = std::unique_ptr<Stmt>;

} // namespace kinii
