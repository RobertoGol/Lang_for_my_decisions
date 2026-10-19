#pragma once
// Лексер KINII: токены с точными координатами (файл, строка, колонка в символах).

#include "kinii/diagnostic.h"
#include "kinii/token.h"

#include <string>
#include <vector>

namespace kinii {

// Разбирает исходник в поток токенов. Ошибки лексера уходят в diags,
// лексер продолжает работу (recovery), чтобы найти максимум проблем за один проход.
std::vector<Token> lex_file(FileId fid, const std::string& src, DiagnosticEngine& diags);

} // namespace kinii
