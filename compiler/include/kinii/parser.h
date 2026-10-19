#pragma once
// Парсер KINII: токены -> AST с подробными диагностиками.

#include "kinii/ast.h"
#include "kinii/diagnostic.h"
#include "kinii/token.h"

#include <vector>

namespace kinii {

// Разбирает поток токенов в дерево. Ошибки уходят в diags с фрагментом кода,
// подсказками и примерами «как должно выглядеть». Парсер восстанавливается
// и ищет максимум ошибок за один проход.
StmtP parse_file(FileId fid, const std::vector<Token>& toks, DiagnosticEngine& diags);

} // namespace kinii
