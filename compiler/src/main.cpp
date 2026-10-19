
// kinic — компилятор языка KINII (VOID-Core v3).
//
// Команды:
//   kinic lex <файлы...>   — показать разбор на токены (для отладки)
//   kinic check <файлы...> — проверить код: парсер + диагностики
//   kinic opt <файлы...>   — оптимизировать проект с отчётом «что и почему»
//   kinic build <файлы...> [-o x.exe] — собрать exe (генерация C++ + g++ -O2)
//   kinic lsp             — языковой сервер для VS Code (JSON-RPC поверх stdio)
//   kinic version          — версия
//
// Код выхода: 0 — ошибок нет, 1 — есть ошибки.

#include "kinii/codegen.h"
#include "kinii/diagnostic.h"
#include "kinii/driver.h"
#include "kinii/lexer.h"
#include "kinii/optimizer.h"
#include "kinii/parser.h"
#include "kinii/sema.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#define KINIC_ISATTY _isatty
#define KINIC_FILENO _fileno
#else
#include <unistd.h>
#define KINIC_ISATTY isatty
#define KINIC_FILENO fileno
#endif

// ---- project.kinii: аналог csproj ----
// Файл в папке проекта:
//   // комментарий
//   files: util.ctos, util2.ctos   (общий код для всех сборок)
//   entry: demo.vc                 (один exe на каждый entry)
// Команды без аргументов работают по этому файлу.

struct KiniiProject {
    bool found = false;
    std::vector<std::string> lib;
    std::vector<std::string> entries;
};

void rtrim(std::string& s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
        s.pop_back();
}

std::string ltrim_str(const std::string& s) {
    size_t k = s.find_first_not_of(" \t");
    return k == std::string::npos ? "" : s.substr(k);
}

bool is_existing_dir(const std::string& path) {
    // директория: аргумент заканчивается на разделитель или реально существует
    if (!path.empty() && (path.back() == '/' || path.back() == '\\')) return true;
    std::error_code ec;
    return std::filesystem::is_directory(path, ec);
}

std::vector<std::string> prefixed(const std::string& dir,
                                  const std::vector<std::string>& v) {
    if (dir.empty() || dir == "." || dir == "./") return v;
    std::vector<std::string> out;
    for (const auto& s : v)
        out.push_back(dir + (dir.back() == '/' ? s : "/" + s));
    return out;
}

KiniiProject load_project_file(const std::string& dir) {
    KiniiProject p;
    // dir может прийти как "examples", "examples/" или empty
    std::string marker;
    if (dir.empty()) marker = "project.kinii";
    else marker = dir + (dir.back() == '/' ? "project.kinii" : "/project.kinii");
    std::ifstream f(marker);
    if (!f) return p;
    p.found = true;
    std::string line;
    while (std::getline(f, line)) {
        rtrim(line);
        std::string t = ltrim_str(line);
        if (t.empty()) continue;
        if (t.rfind("//", 0) == 0 || t[0] == '#') continue;
        size_t colon = t.find(':');
        if (colon == std::string::npos) continue;
        std::string key = t.substr(0, colon);
        std::string rest = t.substr(colon + 1);
        rtrim(key);
        key = ltrim_str(key);
        rtrim(rest);
        if (key == "entry") {
            std::string v = ltrim_str(rest);
            if (!v.empty()) p.entries.push_back(v);
        } else if (key == "files") {
            size_t a = 0;
            while (a <= rest.size()) {
                size_t comma = rest.find(',', a);
                std::string one = rest.substr(a, comma == std::string::npos ? rest.size() - a
                                                                            : comma - a);
                rtrim(one);
                one = ltrim_str(one);
                if (!one.empty()) p.lib.push_back(one);
                if (comma == std::string::npos) break;
                a = comma + 1;
            }
        }
    }
    return p;
}

namespace {

bool read_file(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

bool stdout_is_tty() { return KINIC_ISATTY(KINIC_FILENO(stdout)) != 0; }

void print_help(std::ostream& os) {
    os << "kinic — компилятор KINII (VOID-Core v3)\n"
       << "\n"
       << "Использование:\n"
       << "  kinic lex <файлы...>      разбор на токены (отладка лексера)\n"
       << "  kinic check <файлы...>    проверить проект (лексер + парсер + семантика)\n"
       << "  kinic opt <файлы...>      оптимизировать с отчётом «что и почему»\n"
       << "  kinic build <файлы...>    собрать exe: [-o имя] (g++ внутри)\n"
       << "  kinic lsp                 языковой сервер для VS Code (stdio)\n"
   << "  kinic version             версия компилятора\n"
       << "\n"
       << "Флаги:\n"
       << "  --no-color                отключить цвета (иначе: авто)\n"
       << "  --color                   включить цвета принудительно\n";
}

int cmd_lex(const std::vector<std::string>& files, bool color) {
    kinii::DiagnosticEngine diags;

    for (const auto& path : files) {
        std::string src;
        if (!read_file(path, src)) {
            std::cerr << "kinic: не могу открыть файл: " << path << "\n";
            return 2;
        }
        kinii::FileId fid = diags.add_file(path, src);
        std::vector<kinii::Token> toks = kinii::lex_file(fid, src, diags);

        std::cout << "-- " << path << " : " << toks.size() << " токенов --\n";
        for (const auto& t : toks) {
            std::string text = t.text;
            if (text.size() > 40) text = text.substr(0, 37) + "...";
            for (auto& ch : text)
                if (ch == '\n' || ch == '\t') ch = ' ';
            std::cout << std::setw(5) << t.begin.line << ":" << std::left << std::setw(4)
                      << t.begin.col << std::right << "  " << std::setw(10) << std::left
                      << kinii::tok_name(t.type) << std::right << "  " << text << "\n";
        }
        std::cout << "\n";
    }

    // Показываем накопленные диагностики (ошибки с фрагментами кода).
    diags.render_all(std::cout, color);
    if (diags.error_count() > 0)
        std::cout << "итог: " << diags.error_count() << " ошибок(и)\n";
    if (diags.warning_count() > 0)
        std::cout << "итог: " << diags.warning_count() << " предупреждений(е)\n";
    return diags.had_errors() ? 1 : 0;
}

int cmd_check(const std::vector<std::string>& files, bool color) {
    kinii::DiagnosticEngine diags;
    kinii::SemaResult sema;
    sema.project_wide = true; // файлы проекта видят друг друга без import
    kinii::Driver driver(diags, sema);

    for (const auto& path : files) {
        kinii::Span empty;
        driver.compile(path, empty);
    }
    driver.finalize();

    diags.render_all(std::cout, color);
    if (diags.error_count() > 0)
        std::cout << "итог: " << diags.error_count() << " ошибок(и)\n";
    if (diags.warning_count() > 0)
        std::cout << "итог: " << diags.warning_count() << " предупреждений(е)\n";
    if (diags.error_count() == 0 && diags.warning_count() == 0)
        std::cout << "ошибок нет — код корректен (" << sema.files.size() << " файл(ов) проверено)\n";
    return diags.had_errors() ? 1 : 0;
}

int cmd_opt(const std::vector<std::string>& files, bool color) {
    kinii::DiagnosticEngine diags;
    kinii::SemaResult sema;
    sema.project_wide = true;
    kinii::Driver driver(diags, sema);

    for (const auto& path : files) {
        kinii::Span empty;
        driver.compile(path, empty);
    }
    driver.finalize();

    // Оптимизировать можно только корректный код: сначала проверки.
    if (diags.had_errors()) {
        diags.render_all(std::cout, color);
        std::cout << "итог: сначала исправьте ошибки — оптимизировать некорректный код нельзя\n";
        return 1;
    }

    kinii::OptReport rep;
    for (kinii::FileId fid : driver.order()) {
        kinii::Stmt* ast = driver.ast_of(fid);
        if (!ast) continue;
        size_t before = kinii::count_nodes(ast);
        kinii::optimize_file(ast, diags.file_path(fid), rep);
        size_t after = kinii::count_nodes(ast);
        rep.nodes_before += before;
        rep.nodes_after += after;
    }

    // Совет: импорт, из которого не взято ни одного имени.
    for (kinii::FileId fid : driver.order()) {
        kinii::Stmt* ast = driver.ast_of(fid);
        if (!ast || fid >= sema.files.size()) continue;
        std::set<std::string> used;
        kinii::collect_reads(ast, used);
        const auto& fs = sema.files[fid];
        for (kinii::FileId imp : fs.imported_ids) {
            if (imp >= sema.files.size()) continue;
            const auto& top = sema.files[imp].top_decls;
            if (top.empty()) continue; // пустой файл — про импорт ничего не скажем
            bool any_used = false;
            for (const auto& kv : top)
                if (used.count(kv.first)) { any_used = true; break; }
            if (!any_used) {
                rep.actions.push_back(
                    {fs.path, 0,
                     "импорт не используется: ни одно имя из него в этом файле не нужно",
                     kinii::OptKind::Advice});
            }
        }
    }

    if (rep.actions.empty()) {
        std::cout << "оптимизатор: без изменений — код уже компактен\n";
        return 0;
    }

    // Отчёт по файлам (действия приходят в порядке прохождения).
    std::string cur;
    bool first = true;
    for (const auto& a : rep.actions) {
        if (a.file != cur || first) {
            std::cout << a.file << ":\n";
            cur = a.file;
            first = false;
        }
        if (a.line) std::cout << "  строка " << a.line << ": " << a.text << "\n";
        else std::cout << "  " << a.text << "\n";
    }
    std::cout << "итог: изменений " << rep.actions.size() << " (свёрток: " << rep.folded
              << ", мёртвого кода: " << rep.dead << ", веток: " << rep.branches << ")\n";
    if (rep.nodes_before > rep.nodes_after)
        std::cout << "место: узлов AST " << rep.nodes_before << " → " << rep.nodes_after
                  << " (минус " << (rep.nodes_before - rep.nodes_after) << ")\n";
    else
        std::cout << "место: узлов AST без изменений (" << rep.nodes_before << ")\n";
    return 0;
}

// Сборка: kinic build <файлы...> [-o out.exe]
// Полный конвейер: проверки -> оптимизация -> генерация C++ -> g++ -O2 -> exe.
int cmd_build(const std::vector<std::string>& files, const std::string& out,
              bool color) {
    if (files.empty()) return 0;
    if (!out.empty() && files.size() > 1) {
        std::cerr << "kinic build: -o задаётся для одного входного файла\n";
        return 2;
    }

    kinii::DiagnosticEngine diags;
    kinii::SemaResult sema;
    sema.project_wide = true;
    kinii::Driver driver(diags, sema);

    std::vector<kinii::FileId> entries;
    for (const auto& path : files) {
        kinii::Span empty;
        kinii::FileId fid = driver.compile(path, empty);
        if (fid != kinii::kNoFile) entries.push_back(fid);
    }
    driver.finalize();

    if (diags.had_errors()) {
        diags.render_all(std::cout, color);
        std::cout << "итог: сначала исправьте ошибки — собрать нельзя\n";
        return 1;
    }

    // Оптимизация перед генерацией (правила те же, что у kinic opt).
    kinii::OptReport opt;
    for (kinii::FileId fid : driver.order())
        if (kinii::Stmt* ast = driver.ast_of(fid))
            kinii::optimize_file(ast, diags.file_path(fid), opt);

    // Имя сборки берём у файла с app_entry (в проектах это всегда один входной файл).
    std::string stem;
    for (kinii::FileId fid : entries) {
        const kinii::Stmt* ast = driver.ast_of(fid);
        if (!ast) continue;
        bool has_entry = false;
        for (const auto& s : ast->body)
            if (s && s->kind == kinii::Stmt::AppEntry) { has_entry = true; break; }
        if (has_entry) {
            stem = kinii::stem_of(diags.file_path(fid));
            break;
        }
    }
    if (stem.empty()) stem = kinii::stem_of(diags.file_path(entries.back()));

    // Генерация C++ проекта (все единицы загрузки в одну трансляционную).
    std::filesystem::create_directories("kinic_build");
    std::string cpp_path = "kinic_build/" + stem + ".cpp";
    std::string exe_path =
        out.empty() ? "kinic_build/" + stem + ".exe" : out;

    {
        std::ofstream f(cpp_path, std::ios::binary);
        if (!f) {
            std::cerr << "kinic build: не могу создать " << cpp_path << "\n";
            return 2;
        }
        kinii::CodeGen cg(diags, f);
        cg.begin();
        for (kinii::FileId fid : driver.order())
            cg.declare_types(driver.ast_of(fid));
        for (kinii::FileId fid : driver.order())
            cg.define_globals(driver.ast_of(fid));
        for (kinii::FileId fid : driver.order())
            cg.define_fns(driver.ast_of(fid));
        for (kinii::FileId fid : entries)
            cg.define_entry(driver.ast_of(fid));
        f.close();
        if (!cg.ok()) {
            diags.render_all(std::cout, color);
            std::cout << "итог: " << diags.error_count()
                      << " ошибок(и) в поддержке бэкенда — код не собран\n";
            return 1;
        }
    }

    std::cout << "сгенерирован " << cpp_path << " — запускаю g++...\n";
    std::string cmd = "g++ \"" + cpp_path + "\" -o \"" + exe_path +
                      "\" -std=c++20 -O2 -s";
    int rc = std::system(cmd.c_str());
    if (rc != 0) {
        std::cout << "итог: g++ вернул код " << (rc == -1 ? -1 : rc / 256)
                  << " — см. ошибки выше\n";
        return 1;
    }
    std::cout << "собрано: " << exe_path << "\n";
    return 0;
}

} // namespace

namespace kinii { int cmd_lsp(); } // lsp.cpp: LSP-сервер поверх stdio

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8); // русский текст в консоли Windows
#endif

    std::vector<std::string> args(argv + 1, argv + argc);
    bool color = stdout_is_tty();
    std::vector<std::string> positional;

    for (const auto& a : args) {
        if (a == "--no-color") color = false;
        else if (a == "--color") color = true;
        else positional.push_back(a);
    }

    if (positional.empty()) {
        print_help(std::cout);
        return 0;
    }

    const std::string& cmd = positional[0];
    if (cmd == "version") {
        std::cout << "kinic 0.1.0 (KINII / VOID-Core v3)\n";
        return 0;
    }
    if (cmd == "help" || cmd == "--help" || cmd == "-h") {
        print_help(std::cout);
        return 0;
    }
    if (cmd == "lex") {
        if (positional.size() < 2) {
            std::cerr << "kinic lex: укажите хотя бы один файл\n";
            return 2;
        }
        std::vector<std::string> files(positional.begin() + 1, positional.end());
        return cmd_lex(files, color);
    }
    if (cmd == "check") {
        std::vector<std::string> files;
        for (size_t i = 1; i < positional.size(); ++i) files.push_back(positional[i]);
        // без аргументов — по project.kinii; один аргумент-папка — тоже
        if (files.empty() || (files.size() == 1 && is_existing_dir(files[0]))) {
            std::string dir = files.empty() ? "." : files[0];
            KiniiProject pr = load_project_file(dir);
            if (pr.found) {
                files = prefixed(dir, pr.lib);
                for (const auto& e : prefixed(dir, pr.entries)) files.push_back(e);
            } else if (files.empty()) {
                std::cerr << "kinic check: укажите файлы или создайте project.kinii\n"
                          << "  files: util.ctos, util2.ctos\n"
                          << "  entry: demo.vc\n";
                return 2;
            } else {
                files.clear();
            }
        }
        if (files.empty()) {
            std::cerr << "kinic check: нечего проверять\n";
            return 2;
        }
        return cmd_check(files, color);
    }
    if (cmd == "opt") {
        std::vector<std::string> files;
        for (size_t i = 1; i < positional.size(); ++i) files.push_back(positional[i]);
        if (files.empty() || (files.size() == 1 && is_existing_dir(files[0]))) {
            std::string dir = files.empty() ? "." : files[0];
            KiniiProject pr = load_project_file(dir);
            if (pr.found) {
                files = prefixed(dir, pr.lib);
                for (const auto& e : prefixed(dir, pr.entries)) files.push_back(e);
            } else if (files.empty()) {
                std::cerr << "kinic opt: укажите файлы или создайте project.kinii\n";
                return 2;
            } else {
                files.clear();
            }
        }
        if (files.empty()) {
            std::cerr << "kinic opt: нечего проверять\n";
            return 2;
        }
        return cmd_opt(files, color);
    }
    if (cmd == "build") {
        std::string out;
        std::vector<std::string> files;
        for (size_t i = 1; i < positional.size(); ++i) {
            if (positional[i] == "-o" && i + 1 < positional.size()) {
                out = positional[++i];
            } else {
                files.push_back(positional[i]);
            }
        }
        // без файлов: собираем по project.kinii (каждый entry — свой exe)
        if (files.empty() || (files.size() == 1 && is_existing_dir(files[0]))) {
            std::string dir = files.empty() ? "." : files[0];
            KiniiProject pr = load_project_file(dir);
            if (pr.found && !pr.entries.empty()) {
                if (!out.empty() && pr.entries.size() > 1) {
                    std::cerr << "kinic build: -o задаётся только при одном entry\n";
                    return 2;
                }
                std::string this_out = (!out.empty() && pr.entries.size() == 1) ? out : "";
                for (const auto& e : prefixed(dir, pr.entries)) {
                    std::vector<std::string> one = prefixed(dir, pr.lib);
                    one.push_back(e);
                    int rc = cmd_build(one, this_out, color);
                    if (rc != 0) return rc;
                }
                return 0;
            }
            if (files.empty()) {
                std::cerr << "kinic build: укажите файлы или создайте project.kinii с entry\n";
                return 2;
            }
            files.clear();
        }
        if (files.empty()) {
            std::cerr << "kinic build: укажите хотя бы один файл\n";
            return 2;
        }
        return cmd_build(files, out, color);
    }

    if (cmd == "lsp") {
        // Языковой сервер (VS Code): JSON-RPC 2.0 поверх stdio.
        return kinii::cmd_lsp();
    }

    std::cerr << "kinic: неизвестная команда '" << cmd << "'\n";
    print_help(std::cerr);
    return 2;
}
