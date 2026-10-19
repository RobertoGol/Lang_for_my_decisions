#pragma once
// Форматтер KINII: токен-в-токен перепечатка исходника с аккуратными отступами.

#include "kinii/common.h"

#include <string>

namespace kinii {

// Возвращает отформатированный текст. Диагностики не собирает и не судит —
// качество кода проверяет kinic check, красоту наводит fmt.
std::string format_source(FileId fid, const std::string& src);

} // namespace kinii
