#pragma once
// Токены лексера KINII.

#include "kinii/common.h"
#include <string>

namespace kinii {

enum class Tok : uint8_t {
    // базовые
    Ident, Number, String, SqlTag, Directive, Bridge, At, Comment,
    // ключевые слова
    KwLet, KwConst, KwFn, KwStruct, KwEnum,
    KwNode, KwIsolate, KwInterface, KwPage, KwAppEntry, KwSpawn,
    KwImport, KwFrom, KwAs,
    KwPub, KwPriv, KwAsync, KwAwait, KwEntry, KwHandler,
    KwIf, KwElse, KwWhile, KwLoop, KwFor, KwIn, KwReturn,
    KwMove, KwDecide, KwWeight, KwBreak, KwContinue,
    KwTrue, KwFalse, KwCell, KwType, KwOperator, KwDefine, KwTag, KwAsm,
    // типы
    TyU8, TyU16, TyU32, TyU64, TyString, TyBool, TyPtr, TyVWord, TyVAddr, TyList, TyMap,
    // пунктуация и операторы
    Plus, Minus, Star, Slash, Percent,
    Amp, Pipe, Pipeline, Caret, Tilde, Bang,
    Assign, Eq, NotEq, Lt, Gt, Le, Ge, And, Or, Shl, Shr,
    Arrow, FatArrow, Dot, Comma, Semi, Colon, Question,
    LParen, RParen, LBracket, RBracket, LBrace, RBrace,
    Eof, Unknown
};

struct Token {
    Tok type = Tok::Unknown;
    std::string text;        // точный исходный текст токена
    std::string value;       // для String — декодированное содержимое; для SqlTag — тело запроса
    SourceLocation begin;
    SourceLocation end;
};

const char* tok_name(Tok t);

} // namespace kinii
