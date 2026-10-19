// Движок диагностик KINII: рендер ошибок в стиле rustc.
// Формат: код ошибки, позиция, фрагмент исходника с кареткой, подписи,
// подсказки и пример «как должно выглядеть».

#include "kinii/diagnostic.h"

#include <algorithm>
#include <iomanip>
#include <sstream>

namespace kinii {

// ---------- UTF-8 helpers ----------

static bool is_cont(unsigned char c) { return (c & 0xC0) == 0x80; }

// 0-индексная позиция в «экранной» строке для колонки col (1-индексной, в кодпоинтах).
// Табуляция расширяется до 4 пробелов.
static size_t display_index(const std::string& line, uint32_t col) {
    size_t di = 0;
    uint32_t cp = 1;
    size_t i = 0;
    while (i < line.size() && cp < col) {
        unsigned char c = (unsigned char)line[i];
        if (c == '\t') di += 4;
        else if (!is_cont(c)) di += 1;
        if (!is_cont(c)) cp++;
        i++;
    }
    return di;
}

static uint32_t cp_count(const std::string& s) {
    uint32_t n = 0;
    for (unsigned char c : s)
        if (!is_cont(c)) n++;
    return n;
}

static std::vector<std::string> split_lines(const std::string& s) {
    std::vector<std::string> lines;
    std::string cur;
    for (char c : s) {
        if (c == '\n') {
            lines.push_back(cur);
            cur.clear();
        } else if (c != '\r') {
            cur.push_back(c);
        }
    }
    lines.push_back(cur);
    return lines;
}

// ---------- DiagnosticBuilder ----------

DiagnosticBuilder& DiagnosticBuilder::label(Span s, std::string text, bool primary) {
    d_.labels.push_back({s, std::move(text), primary});
    return *this;
}

DiagnosticBuilder& DiagnosticBuilder::hint(std::string h) {
    d_.hints.push_back(std::move(h));
    return *this;
}

DiagnosticBuilder& DiagnosticBuilder::suggest(std::string s) {
    d_.hints.push_back("возможно, вы имели в виду: " + s);
    return *this;
}

DiagnosticBuilder& DiagnosticBuilder::fix(std::string title, std::string snippet) {
    d_.fix = FixIt{std::move(title), std::move(snippet)};
    return *this;
}

void DiagnosticBuilder::emit() { e_.push(std::move(d_)); }

// ---------- DiagnosticEngine ----------

FileId DiagnosticEngine::add_file(std::string path, std::string source) {
    files_.push_back({std::move(path), std::move(source)});
    return (FileId)(files_.size() - 1);
}

const DiagnosticEngine::FileEnt* DiagnosticEngine::file_entry(FileId id) const {
    if (id >= files_.size()) return nullptr;
    return &files_[id];
}

void DiagnosticEngine::push(Diagnostic d) {
    if (quiet_) return; // тихий сбор таблиц символов (первый проход драйвера)
    if (d.sev == Sev::Error) {
        n_errors_++;
        had_errors_ = true;
    } else if (d.sev == Sev::Warning) {
        n_warnings_++;
    }
    rendered_.push_back(std::move(d));
}

DiagnosticBuilder DiagnosticEngine::error(std::string code, std::string message, Span primary) {
    Diagnostic d;
    d.sev = Sev::Error;
    d.code = std::move(code);
    d.message = std::move(message);
    d.primary = primary;
    d.labels.push_back({primary, "", true});
    return DiagnosticBuilder(*this, std::move(d));
}

DiagnosticBuilder DiagnosticEngine::warning(std::string code, std::string message, Span primary) {
    Diagnostic d;
    d.sev = Sev::Warning;
    d.code = std::move(code);
    d.message = std::move(message);
    d.primary = primary;
    d.labels.push_back({primary, "", true});
    return DiagnosticBuilder(*this, std::move(d));
}

void DiagnosticEngine::render_all(std::ostream& os, bool color) const {
    for (const auto& d : rendered_) render(d, os, color);
}

const std::string& DiagnosticEngine::file_path(FileId id) const {
    static const std::string empty;
    const FileEnt* fe = file_entry(id);
    return fe ? fe->path : empty;
}

// ---------- Рендер ----------

void DiagnosticEngine::render(const Diagnostic& d, std::ostream& os, bool color) const {
    const char* reset = color ? "\x1b[0m" : "";
    const std::string bold = color ? "\x1b[1m" : "";
    const std::string dim = color ? "\x1b[2m" : "";
    const std::string gut = color ? "\x1b[1;34m" : "";   // синий: номера строк и рамка
    const std::string help = color ? "\x1b[1;36m" : "";  // циан: подсказки

    const char* sevname;
    std::string sevcol;
    switch (d.sev) {
        case Sev::Error:   sevname = "error";   sevcol = color ? "\x1b[1;31m" : ""; break;
        case Sev::Warning: sevname = "warning"; sevcol = color ? "\x1b[1;33m" : ""; break;
        case Sev::Note:    sevname = "note";    sevcol = color ? "\x1b[1;35m" : ""; break;
        default:           sevname = "help";    sevcol = color ? "\x1b[1;36m" : ""; break;
    }

    os << sevcol << bold << sevname;
    if (!d.code.empty()) os << "[" << d.code << "]";
    os << reset << ": " << bold << d.message << reset << "\n";

    const FileEnt* fe = file_entry(d.primary.file);
    if (!fe) return;

    auto lines = split_lines(fe->src);

    // Метки в других файлах нельзя показать каретками — выводим их заметками.
    std::vector<const Label*> cross;
    std::vector<const Label*> same;
    for (const auto& l : d.labels) {
        if (l.span.file != kNoFile && l.span.file != d.primary.file) cross.push_back(&l);
        else same.push_back(&l);
    }

    // Собираем все метки (primary + дополнительные).
    std::vector<const Label*> ls = same;
    if (ls.empty()) ls.push_back(nullptr); // только primary span

    // Диапазон строк для показа (многострочные спаны обрезаем до 3 строк).
    uint32_t lo = UINT32_MAX, hi = 0;
    bool any = false;
    for (const Label* l : ls) {
        if (!l) continue;
        uint32_t b = l->span.begin.line;
        uint32_t e = std::min(l->span.end.line, l->span.begin.line + 2);
        if (b == 0 || b > lines.size()) continue;
        lo = std::min(lo, b);
        hi = std::max(hi, std::min<uint32_t>(e, (uint32_t)lines.size()));
        any = true;
    }
    if (!any) {
        lo = hi = std::min<uint32_t>(d.primary.begin.line, (uint32_t)lines.size());
        if (lo == 0) lo = 1;
    }
    int gw = std::to_string(hi).size();

    os << gut << "  --> " << reset << fe->path << ":" << d.primary.begin.line << ":"
       << d.primary.begin.col << "\n";
    os << gut << "   |" << reset << "\n";

    auto on_line = [&](const Label* l, uint32_t ln) {
        return l && ln >= l->span.begin.line &&
               ln <= std::min(l->span.end.line, l->span.begin.line + 2) &&
               l->span.begin.line <= lines.size();
    };

    for (uint32_t ln = lo; ln <= hi; ++ln) {
        const std::string& src = lines[ln - 1];

        os << gut << std::setw(gw) << ln << " |" << reset << " " << src << "\n";

        // Строим строку кареток для всех меток этой строки.
        std::string caret;
        const Label* first = nullptr;
        size_t first_pos = SIZE_MAX;
        for (const Label* l : ls) {
            if (!on_line(l, ln)) continue;
            uint32_t cb = (ln == l->span.begin.line) ? l->span.begin.col : 1;
            uint32_t ce = (ln == l->span.end.line)
                              ? l->span.end.col
                              : (cp_count(src) + 1);
            if (ce < cb) ce = cb;
            size_t p0 = display_index(src, cb);
            size_t p1 = display_index(src, ce);
            if (p1 <= p0) p1 = p0 + 1;
            if (caret.size() < p1) caret.resize(p1, ' ');
            char m = l->primary ? '^' : '-';
            for (size_t i = p0; i < p1; ++i) caret[i] = m;
            if (p0 < first_pos) {
                first_pos = p0;
                first = l;
            }
        }

        if (!caret.empty()) {
            os << gut << std::setw(gw) << "" << " |" << reset << " " << sevcol << caret
               << reset;
            if (first && !first->text.empty()) os << " " << bold << first->text << reset;
            os << "\n";

            // Прочие подписи этой строки — каждая на своей строке под своей колонкой.
            for (const Label* l : ls) {
                if (l == first || !on_line(l, ln) || l->text.empty()) continue;
                uint32_t cb = (ln == l->span.begin.line) ? l->span.begin.col : 1;
                size_t p0 = display_index(src, cb);
                os << gut << std::setw(gw) << "" << " |" << reset << " "
                   << std::string(p0, ' ') << dim << l->text << reset << "\n";
            }
        }
    }

    os << gut << "   |" << reset << "\n";

    // Метки из других файлов — заметками с путём.
    if (!cross.empty()) {
        const std::string notecol = color ? "\x1b[1;35m" : "";
        for (const Label* l : cross) {
            const FileEnt* ce = file_entry(l->span.file);
            os << notecol << "note: " << reset << l->text;
            if (ce)
                os << " (" << ce->path << ":" << l->span.begin.line << ":"
                   << l->span.begin.col << ")";
            os << "\n";
        }
        os << gut << "   |" << reset << "\n";
    }

    for (const auto& h : d.hints) os << help << "help: " << reset << h << "\n";

    if (d.fix) {
        os << help << "help: " << reset << bold << d.fix->title << reset << "\n";
        std::istringstream ss(d.fix->snippet);
        std::string ln2;
        while (std::getline(ss, ln2)) os << "       " << ln2 << "\n";
    }
}

} // namespace kinii
