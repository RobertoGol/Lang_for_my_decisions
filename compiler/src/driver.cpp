// Драйвер: сборка проекта из цепочек import.
// Фаза 1 — загрузка всех файлов графа (лексер + скан импортов + парсер).
//          Импорты ищутся относительно папки текущего файла, затем от корня проекта.
// Фаза 2 (finalize) — семантика в обратном порядке открытия: зависимости раньше
//          зависящих, поэтому ссылки между файлами проверяются корректно.

#include "kinii/driver.h"
#include "kinii/lexer.h"

#include <fstream>
#include <sstream>

namespace kinii {

namespace {

bool read_file_raw(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

// (normalize теперь статический член Driver — см. ниже)

std::string dir_of(const std::string& path) {
    size_t k = path.find_last_of('/');
    if (k == std::string::npos) return "";
    return path.substr(0, k);
}

} // namespace

std::string Driver::normalize(std::string p) {
    for (auto& c : p)
        if (c == '\\') c = '/';
    return p;
}

FileId Driver::compile(const std::string& raw_path, const Span& at_import) {
    std::string path = normalize(raw_path);

    // Содержимое из редактора важнее дискового.
    std::string src;
    auto ov = overrides_.find(path);
    if (ov != overrides_.end()) {
        src = ov->second;
    } else if (!read_file_raw(path, src)) {
        dg_.error("K2012", "файл импорта не найден: '" + path + "'", at_import)
            .hint("импорт ищется относительно папки текущего файла и корня проекта")
            .fix("как должно выглядеть:", "import \"kernel.ctos\";")
            .emit();
        return kNoFile;
    }

    auto it = loaded_.find(path);
    if (it != loaded_.end()) return it->second;

    if (loading_.count(path)) {
        dg_.error("K2009", "круговой импорт: файл '" + path + "' уже собирается", at_import)
            .hint("разорвите цикл взаимных import")
            .emit();
        return kNoFile;
    }

    loading_.insert(path);
    FileId fid = dg_.add_file(path, src);
    std::vector<Token> toks = lex_file(fid, src, dg_);

    // Готовим запись этого файла заранее.
    if (res_.files.size() <= fid) res_.files.resize(fid + 1);
    res_.files[fid].file = fid;
    res_.files[fid].path = path;

    // Скан импортов: KwImport + String (рекурсивно загружаем зависимости).
    for (size_t k = 0; k + 1 < toks.size(); ++k) {
        if (toks[k].type != Tok::KwImport || toks[k + 1].type != Tok::String) continue;
        Span ispan;
        ispan.file = fid;
        ispan.begin = toks[k + 1].begin;
        ispan.end = toks[k + 1].end;
        std::string name = toks[k + 1].value;
        res_.files[fid].imports.push_back(name);

        // Кандидаты: рядом с текущим файлом, затем как есть (от корня проекта).
        std::string d = dir_of(path);
        std::string cand1 = d.empty() ? normalize(name) : normalize(d + "/" + name);
        std::string cand2 = normalize(name);

        std::string resolved;
        std::string probe;
        if (read_file_raw(cand1, probe)) resolved = cand1;
        else if (read_file_raw(cand2, probe)) resolved = cand2;

        if (resolved.empty()) {
            dg_.error("K2012", "файл импорта не найден: '" + name + "'", ispan)
                .hint("ищу в: " + (d.empty() ? std::string("./") : d + "/") + " и " + cand2)
                .hint("проверьте имя файла и его расширение")
                .emit();
            continue;
        }

        FileId child = compile(resolved, ispan);
        // ВАЖНО: compile() мог реаллоцировать res_.files — берём ссылку заново.
        if (child != kNoFile) res_.files[fid].imported_ids.push_back(child);
    }

    Unit u;
    u.fid = fid;
    u.ast = parse_file(fid, toks, dg_);
    units_[fid] = std::move(u);
    order_.push_back(fid);

    loaded_[path] = fid;
    loading_.erase(path);
    return fid;
}

void Driver::finalize() {
    // Проход 1 (тихий): собираем таблицы символов всех файлов в res.global,
    // чтобы ссылки между файлами проверялись с полными знаниями.
    dg_.set_quiet(true);
    for (FileId fid : order_) {
        const Unit& u = units_[fid];
        run_sema(fid, u.ast, dg_, res_);
    }
    dg_.set_quiet(false);

    // Проход 2: сами диагностики — теперь все импорты известны.
    for (FileId fid : order_) {
        const Unit& u = units_[fid];
        run_sema(fid, u.ast, dg_, res_);
    }
}

Stmt* Driver::ast_of(FileId fid) {
    auto it = units_.find(fid);
    return it == units_.end() ? nullptr : it->second.ast.get();
}

} // namespace kinii
