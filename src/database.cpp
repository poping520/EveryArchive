#include "database.h"

#include <windows.h>

#include <cstring>

#include "sqlite3.h"
#include "logger.h"
#include "string_utils.h"

// 使用 string_utils.h 中的 Utf8ToWString / WideToUtf8
// WStringToUtf8 作为 WideToUtf8 的别名，保持内部代码兼容
static inline std::string WStringToUtf8(const std::wstring& w) { return WideToUtf8(w); }

static std::wstring BuildSearchLikePattern(const std::wstring& filter)
{
    std::wstring pattern;
    pattern.reserve(filter.size() + 2);
    pattern.push_back(L'%');

    for (wchar_t ch : filter) {
        switch (ch) {
        case L'*':
            pattern.push_back(L'%');
            break;
        case L'?':
            pattern.push_back(L'_');
            break;
        case L'%':
        case L'_':
        case L'\\':
            pattern.push_back(L'\\');
            pattern.push_back(ch);
            break;
        default:
            pattern.push_back(ch);
            break;
        }
    }

    pattern.push_back(L'%');
    return pattern;
}

static std::string BuildFts5Query(const std::wstring& filter)
{
    if (filter.empty()) return {};

    std::string utf8 = WStringToUtf8(filter);
    std::string result;
    std::string token;

    auto flushToken = [&]() {
        if (token.empty()) return;
        // Strip leading '*' (FTS5 token matching makes it redundant)
        size_t start = 0;
        while (start < token.size() && token[start] == '*') ++start;
        if (start >= token.size()) { token.clear(); return; }
        std::string cleaned = token.substr(start);
        if (!result.empty()) result += " AND ";
        // Keep trailing '*' for FTS5 prefix matching
        result += cleaned;
        token.clear();
    };

    for (char ch : utf8) {
        if (ch == ' ' || ch == '\t') {
            flushToken();
        } else {
            token.push_back(ch);
        }
    }
    flushToken();
    return result;
}

static bool ArchivesTableHasColumn(sqlite3* db, const char* columnName)
{
    if (!db || !columnName) return false;

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "PRAGMA table_info(archives)", -1, &stmt, nullptr) != SQLITE_OK || !stmt) {
        if (stmt) sqlite3_finalize(stmt);
        return false;
    }

    bool found = false;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* name = (const char*)sqlite3_column_text(stmt, 1);
        if (name && std::strcmp(name, columnName) == 0) {
            found = true;
            break;
        }
    }

    sqlite3_finalize(stmt);
    return found;
}

Database::Database() = default;

Database::~Database()
{
    Close();
}

bool Database::Open(const std::wstring& dbPath, std::wstring* err)
{
    if (err) err->clear();

    Close();

    // 转换路径为 UTF-8（sqlite3_open 创建 UTF-8 编码数据库，文本存储减半）
    std::string dbPathUtf8 = WStringToUtf8(dbPath);

    sqlite3* db = nullptr;
    const int rc = sqlite3_open(dbPathUtf8.c_str(), &db);
    if (rc != SQLITE_OK || !db)
    {
        if (err)
        {
            *err = L"sqlite3_open failed";
        }
        if (db)
        {
            sqlite3_close(db);
        }
        return false;
    }

    db_ = db;

    // 性能优化 PRAGMAs（索引数据库可重建，安全性可放宽）
    sqlite3_exec(db_, "PRAGMA journal_mode = WAL", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "PRAGMA synchronous = NORMAL", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "PRAGMA cache_size = -8000", nullptr, nullptr, nullptr);   // 8MB cache
    sqlite3_exec(db_, "PRAGMA temp_store = MEMORY", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "PRAGMA mmap_size = 268435456", nullptr, nullptr, nullptr); // 256MB mmap

    return true;
}

void Database::SetBusyTimeout(int ms)
{
    if (db_) sqlite3_busy_timeout(db_, ms);
}

bool Database::InsertOrUpdateEntry(const ArchiveEntry_t& e)
{
    if (!db_)
    {
        return false;
    }
    if (e.entryPathUtf8.empty())
    {
        return false;
    }

    const char* sql =
        "INSERT INTO entries (archive_id, entry_path, entry_raw_path, compressed_size, original_size, modified_time) "
        "VALUES (?, ?, ?, ?, ?, ?)";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK || !stmt)
    {
        if (stmt) sqlite3_finalize(stmt);
        return false;
    }

    const std::string& pathUtf8 = e.entryPathUtf8;
    sqlite3_bind_int64(stmt, 1, e.archiveId);
    sqlite3_bind_text(stmt, 2, pathUtf8.c_str(), -1, SQLITE_TRANSIENT);
    if (e.entryRawPath.empty()) {
        sqlite3_bind_null(stmt, 3);
    } else {
        sqlite3_bind_blob(stmt, 3, e.entryRawPath.data(), (int)e.entryRawPath.size(), SQLITE_TRANSIENT);
    }
    sqlite3_bind_int64(stmt, 4, (sqlite3_int64)e.compressedSize);
    sqlite3_bind_int64(stmt, 5, (sqlite3_int64)e.originalSize);
    sqlite3_bind_int64(stmt, 6, (sqlite3_int64)e.modifiedTime);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return false;

    if (fts5Available_) {
        sqlite3_stmt* ftsStmt = nullptr;
        const char* ftsSql = "INSERT INTO entries_fts(rowid, entry_path) VALUES (?, ?)";
        if (sqlite3_prepare_v2(db_, ftsSql, -1, &ftsStmt, nullptr) == SQLITE_OK && ftsStmt) {
            sqlite3_bind_int64(ftsStmt, 1, sqlite3_last_insert_rowid(db_));
            sqlite3_bind_text(ftsStmt, 2, pathUtf8.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(ftsStmt);
            sqlite3_finalize(ftsStmt);
        }
    }

    return true;
}

bool Database::InsertEntriesBatch(const std::vector<ArchiveEntry_t>& entries, std::wstring* err)
{
    if (err) err->clear();
    if (!db_)
    {
        if (err) *err = L"db not open";
        return false;
    }
    if (entries.empty()) return true;

    const bool ownsTransaction = sqlite3_get_autocommit(db_) != 0;
    if (ownsTransaction && !BeginTransaction())
    {
        if (err) *err = L"BeginTransaction failed";
        return false;
    }

    const char* sql =
        "INSERT INTO entries (archive_id, entry_path, entry_raw_path, compressed_size, original_size, modified_time) "
        "VALUES (?, ?, ?, ?, ?, ?)";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK || !stmt)
    {
        if (ownsTransaction) RollbackTransaction();
        if (err)
        {
            const char* em = sqlite3_errmsg(db_);
            *err = em ? Utf8ToWString(em) : L"prepare failed";
        }
        if (stmt) sqlite3_finalize(stmt);
        return false;
    }

    for (const auto& e : entries)
    {
        if (e.entryPathUtf8.empty())
        {
            sqlite3_finalize(stmt);
            if (ownsTransaction) RollbackTransaction();
            if (err) *err = L"entry_path is empty";
            return false;
        }

        const std::string& pathUtf8 = e.entryPathUtf8;
        sqlite3_bind_int64(stmt, 1, e.archiveId);
        sqlite3_bind_text(stmt, 2, pathUtf8.c_str(), -1, SQLITE_TRANSIENT);
        if (e.entryRawPath.empty()) {
            sqlite3_bind_null(stmt, 3);
        } else {
            sqlite3_bind_blob(stmt, 3, e.entryRawPath.data(), (int)e.entryRawPath.size(), SQLITE_TRANSIENT);
        }
        sqlite3_bind_int64(stmt, 4, (sqlite3_int64)e.compressedSize);
        sqlite3_bind_int64(stmt, 5, (sqlite3_int64)e.originalSize);
        sqlite3_bind_int64(stmt, 6, (sqlite3_int64)e.modifiedTime);

        rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE)
        {
            if (err)
            {
                const char* em = sqlite3_errmsg(db_);
                *err = em ? Utf8ToWString(em) : L"step failed";
            }
            sqlite3_finalize(stmt);
            if (ownsTransaction) RollbackTransaction();
            return false;
        }
        sqlite3_reset(stmt);
    }

    sqlite3_finalize(stmt);

    if (ownsTransaction && !CommitTransaction())
    {
        RollbackTransaction();
        if (err) *err = L"CommitTransaction failed";
        return false;
    }

    return true;
}

bool Database::QueryEntries(const std::wstring& filter, std::vector<ArchiveEntry_t>* out, std::wstring* err)
{
    if (err) err->clear();
    if (!db_)
    {
        if (err) *err = L"db not open";
        return false;
    }
    if (!out)
    {
        if (err) *err = L"out is null";
        return false;
    }

    out->clear();

    sqlite3_stmt* stmt = nullptr;
    const bool hasFilter = !filter.empty();
    const bool useFts = fts5Available_ && hasFilter;

    const char* sqlLikeFiltered =
        "SELECT a.file_path, e.entry_path, e.compressed_size, e.original_size, e.modified_time "
        "FROM entries e JOIN archives a ON e.archive_id = a.id "
        "WHERE e.entry_path LIKE ? ESCAPE '\\' "
        "ORDER BY e.id DESC;";
    const char* sqlFtsFiltered =
        "SELECT a.file_path, e.entry_path, e.compressed_size, e.original_size, e.modified_time "
        "FROM entries e JOIN archives a ON e.archive_id = a.id "
        "WHERE e.id IN (SELECT rowid FROM entries_fts WHERE entries_fts MATCH ?) "
        "ORDER BY e.id DESC;";
    const char* sqlNoFilter =
        "SELECT a.file_path, e.entry_path, e.compressed_size, e.original_size, e.modified_time "
        "FROM entries e JOIN archives a ON e.archive_id = a.id ORDER BY e.id DESC;";

    const char* sql = hasFilter ? (useFts ? sqlFtsFiltered : sqlLikeFiltered) : sqlNoFilter;

    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK || !stmt)
    {
        if (err)
        {
            const char* em = sqlite3_errmsg(db_);
            *err = em ? Utf8ToWString(em) : L"sqlite3_prepare_v2 failed";
        }
        if (stmt) sqlite3_finalize(stmt);
        return false;
    }

    if (hasFilter)
    {
        std::string filterUtf8 = useFts ? BuildFts5Query(filter) : WStringToUtf8(BuildSearchLikePattern(filter));
        sqlite3_bind_text(stmt, 1, filterUtf8.c_str(), -1, SQLITE_TRANSIENT);
    }

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW)
    {
        ArchiveEntry_t e;

        const char* ap = (const char*)sqlite3_column_text(stmt, 0);
        const char* ep = (const char*)sqlite3_column_text(stmt, 1);
        if (ap) e.archivePath = Utf8ToWString(ap);
        if (ep) e.entryPathUtf8 = ep;

        e.compressedSize = (std::int64_t)sqlite3_column_int64(stmt, 2);
        e.originalSize = (std::uint64_t)sqlite3_column_int64(stmt, 3);
        e.modifiedTime = (std::uint64_t)sqlite3_column_int64(stmt, 4);

        out->push_back(std::move(e));
    }

    if (rc != SQLITE_DONE)
    {
        if (err)
        {
            const char* em = sqlite3_errmsg(db_);
            *err = em ? Utf8ToWString(em) : L"sqlite3_step failed";
        }
        sqlite3_finalize(stmt);
        return false;
    }

    sqlite3_finalize(stmt);
    return true;
}

bool Database::CreateEntriesTable(std::wstring* err)
{
    if (err) err->clear();
    if (!db_)
    {
        if (err) *err = L"db not open";
        return false;
    }

    const char* sql = R"(
            CREATE TABLE IF NOT EXISTS entries (
                id INTEGER PRIMARY KEY,
                archive_id INTEGER NOT NULL,
                entry_path TEXT NOT NULL,
                entry_raw_path BLOB,
                compressed_size INTEGER NOT NULL,
                original_size INTEGER NOT NULL,
                modified_time INTEGER NOT NULL DEFAULT 0
            );
            CREATE INDEX IF NOT EXISTS idx_entries_archive_id ON entries(archive_id);
        )";

    char* errMsg = nullptr;
    const int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK)
    {
        LOG_ERROR(L"Create entries table failed: %s", errMsg);
        sqlite3_free(errMsg);
        return false;
    }
    char* alterErr = nullptr;
    sqlite3_exec(db_, "ALTER TABLE entries ADD COLUMN entry_raw_path BLOB", nullptr, nullptr, &alterErr);
    if (alterErr) sqlite3_free(alterErr);
    alterErr = nullptr;
    sqlite3_exec(db_, "ALTER TABLE entries ADD COLUMN modified_time INTEGER NOT NULL DEFAULT 0", nullptr, nullptr, &alterErr);
    if (alterErr) sqlite3_free(alterErr);

    fts5Available_ = false;
    char* ftsErr = nullptr;
    const char* ftsSql = "CREATE VIRTUAL TABLE IF NOT EXISTS entries_fts"
                         " USING fts5(entry_path, content='entries', content_rowid='id', tokenize='unicode61');";
    if (sqlite3_exec(db_, ftsSql, nullptr, nullptr, &ftsErr) == SQLITE_OK) {
        fts5Available_ = true;
    } else {
        if (ftsErr) sqlite3_free(ftsErr);
    }

    return true;
}

bool Database::DeleteEntriesByArchivePath(const std::wstring& archivePath, std::wstring* err)
{
    if (err) err->clear();
    if (!db_)
    {
        if (err) *err = L"db not open";
        return false;
    }

    const char* sql = "DELETE FROM entries WHERE archive_id IN (SELECT id FROM archives WHERE file_path = ?);";

    if (fts5Available_) {
        const char* ftsSql = "DELETE FROM entries_fts WHERE rowid IN ("
            "SELECT e.id FROM entries e JOIN archives a ON e.archive_id = a.id WHERE a.file_path = ?)";
        sqlite3_stmt* ftsStmt = nullptr;
        if (sqlite3_prepare_v2(db_, ftsSql, -1, &ftsStmt, nullptr) == SQLITE_OK && ftsStmt) {
            std::string pathUtf8Fts = WStringToUtf8(archivePath);
            sqlite3_bind_text(ftsStmt, 1, pathUtf8Fts.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(ftsStmt);
            sqlite3_finalize(ftsStmt);
        }
    }

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK || !stmt)
    {
        if (err)
        {
            const char* em = sqlite3_errmsg(db_);
            *err = em ? Utf8ToWString(em) : L"sqlite3_prepare_v2 failed";
        }
        if (stmt) sqlite3_finalize(stmt);
        return false;
    }

    std::string pathUtf8 = WStringToUtf8(archivePath);
    sqlite3_bind_text(stmt, 1, pathUtf8.c_str(), -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE)
    {
        if (err)
        {
            const char* em = sqlite3_errmsg(db_);
            *err = em ? Utf8ToWString(em) : L"sqlite3_step failed";
        }
        return false;
    }
    return true;
}

bool Database::DeleteEntriesByArchiveId(int64_t archiveId, std::wstring* err)
{
    if (err) err->clear();
    if (!db_)
    {
        if (err) *err = L"db not open";
        return false;
    }

    const char* sql = "DELETE FROM entries WHERE archive_id = ?";

    if (fts5Available_) {
        const char* ftsSql = "DELETE FROM entries_fts WHERE rowid IN (SELECT id FROM entries WHERE archive_id = ?)";
        sqlite3_stmt* ftsStmt = nullptr;
        if (sqlite3_prepare_v2(db_, ftsSql, -1, &ftsStmt, nullptr) == SQLITE_OK && ftsStmt) {
            sqlite3_bind_int64(ftsStmt, 1, archiveId);
            sqlite3_step(ftsStmt);
            sqlite3_finalize(ftsStmt);
        }
    }

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK || !stmt)
    {
        if (err)
        {
            const char* em = sqlite3_errmsg(db_);
            *err = em ? Utf8ToWString(em) : L"prepare failed";
        }
        if (stmt) sqlite3_finalize(stmt);
        return false;
    }

    sqlite3_bind_int64(stmt, 1, archiveId);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE)
    {
        if (err)
        {
            const char* em = sqlite3_errmsg(db_);
            *err = em ? Utf8ToWString(em) : L"step failed";
        }
        return false;
    }
    return true;
}

bool Database::QueryArchives(const std::wstring& filter, std::vector<ArchiveFile_t>* out, std::wstring* err)
{
    if (err) err->clear();
    if (!db_)
    {
        if (err) *err = L"db not open";
        return false;
    }
    if (!out)
    {
        if (err) *err = L"out is null";
        return false;
    }

    out->clear();

    sqlite3_stmt* stmt = nullptr;
    const bool hasFilter = !filter.empty();

    const char* sql = hasFilter
                          ? "SELECT drive_letter, file_path, file_size, modified_time, usn, file_ref_number FROM archives "
                            "WHERE file_path LIKE ? ESCAPE '\\' ORDER BY id DESC;"
                          : "SELECT drive_letter, file_path, file_size, modified_time, usn, file_ref_number FROM archives ORDER BY id DESC;";

    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK || !stmt)
    {
        if (err)
        {
            const char* em = sqlite3_errmsg(db_);
            *err = em ? Utf8ToWString(em) : L"sqlite3_prepare_v2 failed";
        }
        if (stmt) sqlite3_finalize(stmt);
        return false;
    }

    if (hasFilter)
    {
        std::string filterUtf8 = WStringToUtf8(BuildSearchLikePattern(filter));
        sqlite3_bind_text(stmt, 1, filterUtf8.c_str(), -1, SQLITE_TRANSIENT);
    }

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW)
    {
        ArchiveFile_t f;

        const char* drive = (const char*)sqlite3_column_text(stmt, 0);
        const char* path = (const char*)sqlite3_column_text(stmt, 1);
        if (drive) f.driveLetter = Utf8ToWString(drive);
        if (path) f.filePath = Utf8ToWString(path);

        f.fileSize = static_cast<uint64_t>(sqlite3_column_int64(stmt, 2));
        f.modifiedTime = static_cast<uint64_t>(sqlite3_column_int64(stmt, 3));
        f.usn = static_cast<USN>(sqlite3_column_int64(stmt, 4));
        f.fileRefNumber = static_cast<DWORDLONG>(sqlite3_column_int64(stmt, 5));

        out->push_back(std::move(f));
    }

    if (rc != SQLITE_DONE)
    {
        if (err)
        {
            const char* em = sqlite3_errmsg(db_);
            *err = em ? Utf8ToWString(em) : L"sqlite3_step failed";
        }
        sqlite3_finalize(stmt);
        return false;
    }

    sqlite3_finalize(stmt);
    return true;
}

void Database::Close()
{
    if (!db_) return;
    sqlite3_close_v2(db_);
    db_ = nullptr;
}

bool Database::ExecSql16(const std::wstring& sql, std::wstring* err)
{
    if (err) err->clear();
    if (!db_)
    {
        if (err) *err = L"db not open";
        return false;
    }

    std::string sqlUtf8 = WStringToUtf8(sql);
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sqlUtf8.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK || !stmt)
    {
        if (err)
        {
            const char* em = sqlite3_errmsg(db_);
            *err = em ? Utf8ToWString(em) : L"sqlite3_prepare_v2 failed";
        }
        if (stmt) sqlite3_finalize(stmt);
        return false;
    }

    for (;;)
    {
        rc = sqlite3_step(stmt);
        if (rc == SQLITE_ROW)
        {
            continue;
        }
        if (rc == SQLITE_DONE)
        {
            break;
        }

        if (err)
        {
            const char* em = sqlite3_errmsg(db_);
            *err = em ? Utf8ToWString(em) : L"sqlite3_step failed";
        }
        sqlite3_finalize(stmt);
        return false;
    }

    sqlite3_finalize(stmt);
    return true;
}

bool Database::CreateConfigsTable(std::wstring* err)
{
    if (err) err->clear();
    if (!db_)
    {
        if (err) *err = L"db not open";
        return false;
    }

    const char* sql = R"(
            CREATE TABLE IF NOT EXISTS configs (
                key TEXT PRIMARY KEY,
                value TEXT NOT NULL
            );
        )";

    char* errMsg = nullptr;
    int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK)
    {
        LOG_ERROR(L"Create configs table failed: %s", errMsg);
        sqlite3_free(errMsg);
        return false;
    }
    return true;
}

bool Database::SaveJournalUsn(wchar_t driveLetter, int64_t journalId, USN nextUsn, std::wstring* err)
{
    if (err) err->clear();
    if (!db_)
    {
        if (err) *err = L"db not open";
        return false;
    }

    // 保存 journal_id 和 next_usn，key 格式: "usn_E" (盘符)
    std::string driveStr(1, (char)driveLetter);
    std::string keyJournal = "journal_id_" + driveStr;
    std::string keyUsn = "next_usn_" + driveStr;

    const char* sql = "INSERT OR REPLACE INTO configs (key, value) VALUES (?, ?)";
    sqlite3_stmt* stmt = nullptr;

    // 保存 journal_id
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) { if (err) *err = L"prepare failed"; return false; }
    sqlite3_bind_text(stmt, 1, keyJournal.c_str(), -1, SQLITE_TRANSIENT);
    std::string valJournal = std::to_string(journalId);
    sqlite3_bind_text(stmt, 2, valJournal.c_str(), -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) { if (err) *err = L"step failed"; return false; }

    // 保存 next_usn
    rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) { if (err) *err = L"prepare failed"; return false; }
    sqlite3_bind_text(stmt, 1, keyUsn.c_str(), -1, SQLITE_TRANSIENT);
    std::string valUsn = std::to_string((long long)nextUsn);
    sqlite3_bind_text(stmt, 2, valUsn.c_str(), -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) { if (err) *err = L"step failed"; return false; }

    return true;
}

bool Database::SaveConfigValue(const std::string& key, const std::string& value, std::wstring* err)
{
    if (err) err->clear();
    if (!db_) {
        if (err) *err = L"db not open";
        return false;
    }

    const char* sql = "INSERT OR REPLACE INTO configs (key, value) VALUES (?, ?)";
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK || !stmt) {
        if (err) {
            const char* em = sqlite3_errmsg(db_);
            *err = em ? Utf8ToWString(em) : L"prepare failed";
        }
        if (stmt) sqlite3_finalize(stmt);
        return false;
    }

    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, value.c_str(), -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        if (err) {
            const char* em = sqlite3_errmsg(db_);
            *err = em ? Utf8ToWString(em) : L"step failed";
        }
        return false;
    }
    return true;
}

bool Database::GetConfigValue(const std::string& key, std::string* outValue)
{
    if (!db_ || !outValue) return false;
    outValue->clear();

    const char* sql = "SELECT value FROM configs WHERE key = ?";
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK || !stmt) {
        if (stmt) sqlite3_finalize(stmt);
        return false;
    }

    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* val = (const char*)sqlite3_column_text(stmt, 0);
        if (val) {
            *outValue = val;
            found = true;
        }
    }
    sqlite3_finalize(stmt);
    return found;
}

bool Database::GetJournalUsn(wchar_t driveLetter, int64_t* outJournalId, USN* outNextUsn)
{
    if (!db_) return false;
    if (outJournalId) *outJournalId = 0;
    if (outNextUsn) *outNextUsn = 0;

    std::string driveStr(1, (char)driveLetter);

    const char* sql = "SELECT value FROM configs WHERE key = ?";
    sqlite3_stmt* stmt = nullptr;

    // 读取 journal_id
    std::string keyJournal = "journal_id_" + driveStr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, keyJournal.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
        const char* val = (const char*)sqlite3_column_text(stmt, 0);
        if (val && outJournalId) *outJournalId = _atoi64(val);
    }
    sqlite3_finalize(stmt);

    // 读取 next_usn
    std::string keyUsn = "next_usn_" + driveStr;
    rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, keyUsn.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
        const char* val = (const char*)sqlite3_column_text(stmt, 0);
        if (val && outNextUsn) *outNextUsn = (USN)_atoi64(val);
    }
    sqlite3_finalize(stmt);

    return true;
}

bool Database::QueryArchiveByRefNumber(wchar_t driveLetter, uint64_t fileRefNumber, ArchiveFile_t* out)
{
    if (!db_ || !out) return false;

    const char* sql = "SELECT drive_letter, file_path, file_size, modified_time, usn, file_ref_number "
                      "FROM archives WHERE drive_letter = ? AND file_ref_number = ?";
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return false;

    std::string driveStr(1, (char)driveLetter);
    sqlite3_bind_text(stmt, 1, driveStr.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, (sqlite3_int64)fileRefNumber);

    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
        const char* drive = (const char*)sqlite3_column_text(stmt, 0);
        const char* path = (const char*)sqlite3_column_text(stmt, 1);
        if (drive) out->driveLetter = Utf8ToWString(drive);
        if (path) out->filePath = Utf8ToWString(path);
        out->fileSize = (uint64_t)sqlite3_column_int64(stmt, 2);
        out->modifiedTime = (uint64_t)sqlite3_column_int64(stmt, 3);
        out->usn = (USN)sqlite3_column_int64(stmt, 4);
        out->fileRefNumber = (DWORDLONG)sqlite3_column_int64(stmt, 5);
        found = true;
    }
    sqlite3_finalize(stmt);
    return found;
}

bool Database::CreateArchivesTable(std::wstring* err)
{
    if (err) err->clear();
    if (!db_)
    {
        if (err) *err = L"db not open";
        return false;
    }

    const char* createSql = R"(
            CREATE TABLE IF NOT EXISTS archives (
                id INTEGER PRIMARY KEY,
                drive_letter TEXT NOT NULL,
                file_ref_number INTEGER NOT NULL,
                usn INTEGER NOT NULL,
                file_path TEXT NOT NULL,
                file_size INTEGER NOT NULL,
                modified_time INTEGER NOT NULL,
                UNIQUE(drive_letter, file_ref_number)
            );
        )";

    char* errMsg = nullptr;
    int rc = sqlite3_exec(db_, createSql, nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK)
    {
        LOG_ERROR(L"Create table failed: %s", errMsg);
        sqlite3_free(errMsg);
        return false;
    }

    if (ArchivesTableHasColumn(db_, "file_name") ||
        ArchivesTableHasColumn(db_, "parent_file_ref_number")) {
        const char* migrateSql = R"(
            BEGIN IMMEDIATE;
            DROP INDEX IF EXISTS idx_file_name;
            CREATE TABLE archives_new (
                id INTEGER PRIMARY KEY,
                drive_letter TEXT NOT NULL,
                file_ref_number INTEGER NOT NULL,
                usn INTEGER NOT NULL,
                file_path TEXT NOT NULL,
                file_size INTEGER NOT NULL,
                modified_time INTEGER NOT NULL,
                UNIQUE(drive_letter, file_ref_number)
            );
            INSERT INTO archives_new
                (id, drive_letter, file_ref_number, usn, file_path, file_size, modified_time)
                SELECT id, drive_letter, file_ref_number, usn, file_path, file_size, modified_time
                FROM archives;
            DROP TABLE archives;
            ALTER TABLE archives_new RENAME TO archives;
            COMMIT;
        )";

        errMsg = nullptr;
        rc = sqlite3_exec(db_, migrateSql, nullptr, nullptr, &errMsg);
        if (rc != SQLITE_OK)
        {
            LOG_ERROR(L"Migrate archives table failed: %s", errMsg);
            sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
            sqlite3_free(errMsg);
            return false;
        }
    } else {
        sqlite3_exec(db_, "DROP INDEX IF EXISTS idx_file_name", nullptr, nullptr, nullptr);
    }

    const char* indexSql = R"(
            CREATE INDEX IF NOT EXISTS idx_drive_letter ON archives(drive_letter);
            CREATE INDEX IF NOT EXISTS idx_usn ON archives(usn);
            CREATE INDEX IF NOT EXISTS idx_archives_file_path ON archives(file_path);
        )";

    errMsg = nullptr;
    rc = sqlite3_exec(db_, indexSql, nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK)
    {
        LOG_ERROR(L"Create archives indexes failed: %s", errMsg);
        sqlite3_free(errMsg);
        return false;
    }
    return true;
}

bool Database::InsertOrUpdateArchive(const ArchiveFile_t& af)
{
    const char* sql = R"(
        INSERT INTO archives
            (drive_letter, file_ref_number, usn, file_path, file_size, modified_time)
            VALUES (?, ?, ?, ?, ?, ?)
            ON CONFLICT(drive_letter, file_ref_number) DO UPDATE SET
                usn=excluded.usn,
                file_path=excluded.file_path,
                file_size=excluded.file_size,
                modified_time=excluded.modified_time
        )";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK)
    {
        LOG_ERROR(L"Prepare failed: %s", Utf8ToWString(sqlite3_errmsg(db_)).c_str());
        return false;
    }

    std::string driveUtf8 = WStringToUtf8(af.driveLetter);
    std::string pathUtf8 = WStringToUtf8(af.filePath);
    sqlite3_bind_text(stmt, 1, driveUtf8.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, af.fileRefNumber);
    sqlite3_bind_int64(stmt, 3, af.usn);
    sqlite3_bind_text(stmt, 4, pathUtf8.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 5, af.fileSize);
    sqlite3_bind_int64(stmt, 6, af.modifiedTime);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE)
    {
        LOG_ERROR(L"Insert/Update failed: %s", Utf8ToWString(sqlite3_errmsg(db_)).c_str());
        return false;
    }
    return true;
}

bool Database::InsertArchivesBatch(const std::vector<ArchiveFile_t>& files, std::wstring* err)
{
    if (err) err->clear();
    if (!db_)
    {
        if (err) *err = L"db not open";
        return false;
    }
    if (files.empty()) return true;

    if (!BeginTransaction())
    {
        if (err) *err = L"BeginTransaction failed";
        return false;
    }

    const char* sql = R"(
        INSERT INTO archives
        (drive_letter, file_ref_number, usn, file_path, file_size, modified_time)
        VALUES (?, ?, ?, ?, ?, ?)
        ON CONFLICT(drive_letter, file_ref_number) DO UPDATE SET
            usn=excluded.usn,
            file_path=excluded.file_path,
            file_size=excluded.file_size,
            modified_time=excluded.modified_time
    )";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK || !stmt)
    {
        RollbackTransaction();
        if (err)
        {
            const char* em = sqlite3_errmsg(db_);
            *err = em ? Utf8ToWString(em) : L"prepare failed";
        }
        if (stmt) sqlite3_finalize(stmt);
        return false;
    }

    for (const auto& af : files)
    {
        std::string driveUtf8 = WStringToUtf8(af.driveLetter);
        std::string pathUtf8 = WStringToUtf8(af.filePath);
        sqlite3_bind_text(stmt, 1, driveUtf8.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 2, af.fileRefNumber);
        sqlite3_bind_int64(stmt, 3, af.usn);
        sqlite3_bind_text(stmt, 4, pathUtf8.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 5, af.fileSize);
        sqlite3_bind_int64(stmt, 6, af.modifiedTime);

        rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE)
        {
            if (err)
            {
                const char* em = sqlite3_errmsg(db_);
                *err = em ? Utf8ToWString(em) : L"step failed";
            }
            sqlite3_finalize(stmt);
            RollbackTransaction();
            return false;
        }
        sqlite3_reset(stmt);
    }

    sqlite3_finalize(stmt);

    if (!CommitTransaction())
    {
        RollbackTransaction();
        if (err) *err = L"CommitTransaction failed";
        return false;
    }

    return true;
}

bool Database::GetArchiveLastUsn(wchar_t driveLetter, USN* outUsn)
{
    const char* sql = "SELECT MAX(usn) FROM archives WHERE drive_letter = ?";
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK)
    {
        return false;
    }

    std::string driveStr(1, (char)driveLetter);
    sqlite3_bind_text(stmt, 1, driveStr.c_str(), -1, SQLITE_TRANSIENT);

    *outUsn = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
        *outUsn = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return true;
}

bool Database::DeleteArchiveByRefNumber(wchar_t driveLetter, uint64_t fileRefNumber)
{
    if (!db_) {
        return false;
    }

    const char* deleteEntriesSql = R"(
        DELETE FROM entries
        WHERE archive_id IN (
            SELECT id FROM archives WHERE drive_letter = ? AND file_ref_number = ?
        );
    )";

    if (fts5Available_) {
        const char* ftsSql = "DELETE FROM entries_fts WHERE rowid IN ("
            "SELECT e.id FROM entries e JOIN archives a ON e.archive_id = a.id "
            "WHERE a.drive_letter = ? AND a.file_ref_number = ?)";
        sqlite3_stmt* ftsStmt = nullptr;
        if (sqlite3_prepare_v2(db_, ftsSql, -1, &ftsStmt, nullptr) == SQLITE_OK && ftsStmt) {
            std::string driveStrFts(1, (char)driveLetter);
            sqlite3_bind_text(ftsStmt, 1, driveStrFts.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(ftsStmt, 2, fileRefNumber);
            sqlite3_step(ftsStmt);
            sqlite3_finalize(ftsStmt);
        }
    }

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, deleteEntriesSql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK)
    {
        return false;
    }

    std::string driveStr(1, (char)driveLetter);
    sqlite3_bind_text(stmt, 1, driveStr.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, fileRefNumber);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        return false;
    }

    const char* deleteArchiveSql = "DELETE FROM archives WHERE drive_letter = ? AND file_ref_number = ?";
    stmt = nullptr;
    rc = sqlite3_prepare_v2(db_, deleteArchiveSql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK)
    {
        return false;
    }

    sqlite3_bind_text(stmt, 1, driveStr.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, fileRefNumber);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

int64_t Database::GetArchiveCount()
{
    const char* sql = "SELECT COUNT(*) FROM archives";
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK)
    {
        return 0;
    }

    int64_t count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
        count = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return count;
}

int64_t Database::GetArchiveIdByPath(const std::wstring& filePath)
{
    if (!db_) return -1;

    const char* sql = "SELECT id FROM archives WHERE file_path = ?";
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return -1;

    std::string pathUtf8 = WStringToUtf8(filePath);
    sqlite3_bind_text(stmt, 1, pathUtf8.c_str(), -1, SQLITE_TRANSIENT);

    int64_t id = -1;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        id = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return id;
}


bool Database::QueryEntryIds(const std::wstring& filter, int sortColumn, bool sortAsc,
                             std::vector<int64_t>* outIds, std::wstring* err)
{
    if (err) err->clear();
    if (!db_) { if (err) *err = L"db not open"; return false; }
    if (!outIds) { if (err) *err = L"outIds is null"; return false; }
    outIds->clear();

    // 构建 ORDER BY 子句
    // 列0(名称): 从 entry_path 提取文件名排序
    // 列1(归档文件): 需要 JOIN archives 表
    const char* orderCol = "e.id";
    bool compressedSizeSort = false;
    switch (sortColumn) {
    case 0: orderCol = "SUBSTR(e.entry_path, LENGTH(RTRIM(e.entry_path, REPLACE(e.entry_path, '/', ''))) + 1)"; break;
    case 1: orderCol = "a.file_path"; break;
    case 2: orderCol = "e.entry_path"; break;
    case 3: orderCol = "e.compressed_size"; compressedSizeSort = true; break;
    case 4: orderCol = "e.original_size"; break;
    case 5: orderCol = "e.modified_time"; break;
    default: orderCol = "e.id"; break;
    }
    const char* orderDir = sortAsc ? "ASC" : "DESC";

    std::string sql;
    const bool hasFilter = !filter.empty();
    const bool useFts = fts5Available_ && hasFilter;
    sql = std::string("SELECT e.id FROM entries e JOIN archives a ON e.archive_id = a.id ");
    if (hasFilter) {
        if (useFts) {
            sql += "WHERE e.id IN (SELECT rowid FROM entries_fts WHERE entries_fts MATCH ?1) ";
        } else {
            sql += "WHERE e.entry_path LIKE ?1 ESCAPE '\\' ";
        }
    }
    if (compressedSizeSort) {
        sql += std::string("ORDER BY CASE WHEN e.compressed_size < 0 THEN 1 ELSE 0 END ASC, ") + orderCol + " " + orderDir;
    } else {
        sql += std::string("ORDER BY ") + orderCol + " " + orderDir;
    }

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK || !stmt) {
        if (err) {
            const char* em = sqlite3_errmsg(db_);
            *err = em ? Utf8ToWString(em) : L"prepare failed";
        }
        if (stmt) sqlite3_finalize(stmt);
        return false;
    }

    if (hasFilter) {
        std::string filterUtf8;
        if (useFts) {
            filterUtf8 = BuildFts5Query(filter);
        } else {
            std::wstring pattern = BuildSearchLikePattern(filter);
            int needed = WideCharToMultiByte(CP_UTF8, 0, pattern.c_str(), -1, nullptr, 0, nullptr, nullptr);
            filterUtf8.resize(needed > 0 ? needed - 1 : 0, '\0');
            if (needed > 0) WideCharToMultiByte(CP_UTF8, 0, pattern.c_str(), -1, filterUtf8.data(), needed, nullptr, nullptr);
        }
        sqlite3_bind_text(stmt, 1, filterUtf8.c_str(), -1, SQLITE_TRANSIENT);
    }

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        outIds->push_back(sqlite3_column_int64(stmt, 0));
    }

    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool Database::QueryEntryById(int64_t rowId, ArchiveEntry_t* out)
{
    if (!db_ || !out) return false;

    const char* sql = "SELECT a.file_path, e.entry_path, e.entry_raw_path, e.compressed_size, e.original_size, e.modified_time "
                      "FROM entries e JOIN archives a ON e.archive_id = a.id WHERE e.id = ?";
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return false;

    sqlite3_bind_int64(stmt, 1, rowId);

    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* ap = (const char*)sqlite3_column_text(stmt, 0);
        const char* ep = (const char*)sqlite3_column_text(stmt, 1);
        if (ap) out->archivePath = Utf8ToWString(ap);
        if (ep) out->entryPathUtf8 = ep;
        const void* raw = sqlite3_column_blob(stmt, 2);
        int rawBytes = sqlite3_column_bytes(stmt, 2);
        if (raw && rawBytes > 0) {
            out->entryRawPath.assign((const char*)raw, rawBytes);
        }
        out->compressedSize = (int64_t)sqlite3_column_int64(stmt, 3);
        out->originalSize = (uint64_t)sqlite3_column_int64(stmt, 4);
        out->modifiedTime = (uint64_t)sqlite3_column_int64(stmt, 5);
        found = true;
    }
    sqlite3_finalize(stmt);
    return found;
}

int64_t Database::GetEntryCount(const std::wstring& filter)
{
    if (!db_) return 0;

    std::string sql;
    const bool hasFilter = !filter.empty();
    const bool useFts = fts5Available_ && hasFilter;
    if (hasFilter) {
        if (useFts) {
            sql = "SELECT COUNT(*) FROM entries e JOIN archives a ON e.archive_id = a.id "
                  "WHERE e.id IN (SELECT rowid FROM entries_fts WHERE entries_fts MATCH ?1)";
        } else {
            sql = "SELECT COUNT(*) FROM entries e JOIN archives a ON e.archive_id = a.id WHERE e.entry_path LIKE ?1 ESCAPE '\\'";
        }
    } else {
        sql = "SELECT COUNT(*) FROM entries e JOIN archives a ON e.archive_id = a.id";
    }

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return 0;

    if (hasFilter) {
        std::string filterUtf8;
        if (useFts) {
            filterUtf8 = BuildFts5Query(filter);
        } else {
            std::wstring pattern = BuildSearchLikePattern(filter);
            int needed = WideCharToMultiByte(CP_UTF8, 0, pattern.c_str(), -1, nullptr, 0, nullptr, nullptr);
            filterUtf8.resize(needed > 0 ? needed - 1 : 0, '\0');
            if (needed > 0) WideCharToMultiByte(CP_UTF8, 0, pattern.c_str(), -1, filterUtf8.data(), needed, nullptr, nullptr);
        }
        sqlite3_bind_text(stmt, 1, filterUtf8.c_str(), -1, SQLITE_TRANSIENT);
    }

    int64_t count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return count;
}

bool Database::BeginTransaction()
{
    char* errMsg = nullptr;
    int rc = sqlite3_exec(db_, "BEGIN TRANSACTION", nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK)
    {
        LOG_ERROR(L"Begin transaction failed: %s", Utf8ToWString(errMsg).c_str());
        sqlite3_free(errMsg);
        return false;
    }
    return true;
}

bool Database::CommitTransaction()
{
    char* errMsg = nullptr;
    int rc = sqlite3_exec(db_, "COMMIT", nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK)
    {
        LOG_ERROR(L"Commit failed: %s", Utf8ToWString(errMsg).c_str());
        sqlite3_free(errMsg);
        return false;
    }
    return true;
}

bool Database::RollbackTransaction()
{
    char* errMsg = nullptr;
    sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, &errMsg);
    if (errMsg) sqlite3_free(errMsg);
    return true;
}

bool Database::Vacuum()
{
    if (!db_) return false;
    char* errMsg = nullptr;
    int rc = sqlite3_exec(db_, "VACUUM", nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK)
    {
        if (errMsg) sqlite3_free(errMsg);
        return false;
    }
    return true;
}

bool Database::RebuildFtsIndex(std::wstring* err)
{
    if (err) err->clear();
    if (!db_) { if (err) *err = L"db not open"; return false; }
    if (!fts5Available_) return true;

    char* errMsg = nullptr;
    int rc = sqlite3_exec(db_, "INSERT INTO entries_fts(entries_fts) VALUES('rebuild')", nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        if (err) {
            *err = errMsg ? Utf8ToWString(errMsg) : L"FTS rebuild failed";
        }
        if (errMsg) sqlite3_free(errMsg);
        return false;
    }
    return true;
}
