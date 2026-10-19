#pragma once
// Базовые типы: координаты в исходнике и диапазон (span).
// col считается в СИМВОЛАХ (кодпоинтах), а не в байтах — чтобы
// ошибки показывали правильную позицию в кириллическом коде.

#include <cstdint>

namespace kinii {

using FileId = uint32_t;
inline constexpr FileId kNoFile = 0xFFFFFFFFu;

struct SourceLocation {
    uint32_t line = 1;    // с 1
    uint32_t col = 1;     // с 1, в кодпоинтах
    uint32_t offset = 0;  // смещение в байтах от начала файла
};

struct Span {
    FileId file = kNoFile;
    SourceLocation begin;
    SourceLocation end;
};

} // namespace kinii
