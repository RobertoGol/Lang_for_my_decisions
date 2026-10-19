#pragma once
// Движок диагностик: ошибки уровня rustc — с фрагментом кода, кареткой,
// подписями, подсказками «как должно выглядеть» и ссылками на другие файлы.

#include "kinii/common.h"
#include <cstdint>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

namespace kinii {

enum class Sev : uint8_t { Error, Warning, Note, Help };

struct Label {
    Span span;
    std::string text;   // подпись к фрагменту (может быть пустой)
    bool primary = false;
};

struct FixIt {
    std::string title;    // например: "как должно выглядеть:"
    std::string snippet;  // пример правильного кода
};

struct Diagnostic {
    Sev sev = Sev::Error;
    std::string code;     // "K0002"
    std::string message;
    Span primary;                       // для строки "--> file:line:col"
    std::vector<Label> labels;          // все подсвеченные фрагменты (в т.ч. в других файлах)
    std::vector<std::string> hints;     // строки "help: ..."
    std::optional<FixIt> fix;
};

class DiagnosticEngine;

class DiagnosticBuilder {
public:
    DiagnosticBuilder(DiagnosticEngine& e, Diagnostic d) : e_(e), d_(std::move(d)) {}

    // Дополнительный подсвеченный фрагмент (можно в другом файле — так работают
    // ссылки вида «импортировано здесь: kernel.ctos:5»).
    DiagnosticBuilder& label(Span s, std::string text, bool primary = false);
    DiagnosticBuilder& hint(std::string h);
    DiagnosticBuilder& suggest(std::string s); // «возможно, вы имели в виду ...»
    DiagnosticBuilder& fix(std::string title, std::string snippet);
    void emit();

private:
    DiagnosticEngine& e_;
    Diagnostic d_;
};

class DiagnosticEngine {
public:
    FileId add_file(std::string path, std::string source);

    DiagnosticBuilder error(std::string code, std::string message, Span primary);
    DiagnosticBuilder warning(std::string code, std::string message, Span primary);

    void render(const Diagnostic& d, std::ostream& os, bool color) const;
    void render_all(std::ostream& os, bool color) const;

    const std::string& file_path(FileId id) const;

    bool had_errors() const { return had_errors_; }
    size_t error_count() const { return n_errors_; }
    size_t warning_count() const { return n_warnings_; }

    // Все накопленные диагностики (для LSP и прочих бэкендов отчётов).
    const std::vector<Diagnostic>& all() const { return rendered_; }

    void set_quiet(bool q) { quiet_ = q; }

    void push(Diagnostic d);

private:
    struct FileEnt {
        std::string path;
        std::string src;
    };
    const FileEnt* file_entry(FileId id) const;

    std::vector<FileEnt> files_;
    std::vector<Diagnostic> rendered_;
    size_t n_errors_ = 0;
    size_t n_warnings_ = 0;
    bool had_errors_ = false;
    bool quiet_ = false;
};

} // namespace kinii
