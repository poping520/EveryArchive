#include "index_store.h"

#include "database.h"
#include "ezdb.h"
#include "string_utils.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <algorithm>
#include <cstdlib>
#include <utility>
#include <unordered_map>

namespace {

void SetErr(std::wstring* err, const std::wstring& value)
{
    if (err) *err = value;
}

std::wstring LastErrorText(const wchar_t* action)
{
    return std::wstring(action) + L" failed, GetLastError=" + std::to_wstring(GetLastError());
}

bool FileExistsW(const std::wstring& path)
{
    DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

bool MoveReplace(const std::wstring& from, const std::wstring& to, std::wstring* err)
{
    if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return true;
    }
    SetErr(err, LastErrorText(L"MoveFileExW"));
    return false;
}

bool BuildEmptyEzdb(const std::wstring& path, std::wstring* err)
{
    const std::wstring tmp = path + L".tmp";
    DeleteFileW(tmp.c_str());
    const std::string tmpUtf8 = WideToUtf8(tmp);
    int rc = ezdb_build_snapshot(nullptr, 0, nullptr, 0, tmpUtf8.c_str());
    if (rc != 0) {
        SetErr(err, Utf8ToWString(ezdb_error_message(rc)));
        DeleteFileW(tmp.c_str());
        return false;
    }
    return MoveReplace(tmp, path, err);
}

ArchiveFile_t ArchiveFromEzdb(const EzdbArchiveResult& result)
{
    ArchiveFile_t out;
    out.driveLetter = result.drive_letter ? std::wstring(1, static_cast<wchar_t>(result.drive_letter)) : std::wstring();
    out.filePath = Utf8ToWString(result.file_path);
    out.fileSize = result.file_size;
    out.modifiedTime = result.modified_time;
    out.fileRefNumber = static_cast<DWORDLONG>(result.file_ref_number);
    out.usn = static_cast<USN>(result.usn);
    return out;
}

ArchiveEntry_t EntryFromEzdb(const EzdbEntryResult& result)
{
    ArchiveEntry_t out;
    out.archiveId = result.archive_id;
    out.archivePath = Utf8ToWString(result.archive_path);
    out.entryPathUtf8 = result.entry_path ? result.entry_path : "";
    if (result.entry_raw_path && result.entry_raw_path_len) {
        out.entryRawPath.assign(static_cast<const char*>(result.entry_raw_path), result.entry_raw_path_len);
    }
    out.compressedSize = result.compressed_size;
    out.originalSize = result.original_size;
    out.modifiedTime = result.modified_time;
    return out;
}

EzdbArchiveRecord MakeEzdbArchiveRecord(const ArchiveFile_t& in, std::string* pathUtf8)
{
    *pathUtf8 = WideToUtf8(in.filePath);
    EzdbArchiveRecord out{};
    out.drive_letter = in.driveLetter.empty() ? 0 : static_cast<char>(in.driveLetter[0]);
    out.file_ref_number = static_cast<uint64_t>(in.fileRefNumber);
    out.usn = static_cast<int64_t>(in.usn);
    out.file_path = pathUtf8->c_str();
    out.file_size = in.fileSize;
    out.modified_time = in.modifiedTime;
    return out;
}

const char* BaseNameUtf8(const std::string& path)
{
    const char* name = path.c_str();
    for (const char* p = name; *p; ++p) {
        if (*p == '/' || *p == '\\') name = p + 1;
    }
    return name;
}

int CompareInt64(int64_t lhs, int64_t rhs)
{
    if (lhs < rhs) return -1;
    if (lhs > rhs) return 1;
    return 0;
}

int CompareUInt64(uint64_t lhs, uint64_t rhs)
{
    if (lhs < rhs) return -1;
    if (lhs > rhs) return 1;
    return 0;
}

void SortEntriesForQuery(std::vector<StoreEntryId>* ids,
                         const std::vector<ArchiveEntry_t>& rows,
                         int sortColumn,
                         bool sortAsc)
{
    if (!ids || ids->size() < 2) return;
    std::vector<size_t> order(ids->size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;

    auto compareText = [](const std::string& a, const std::string& b) {
        return a.compare(b);
    };

    std::sort(order.begin(), order.end(), [&](size_t lhs, size_t rhs) {
        const ArchiveEntry_t& a = rows[lhs];
        const ArchiveEntry_t& b = rows[rhs];
        int cmp = 0;
        switch (sortColumn) {
        case 0:
            cmp = compareText(BaseNameUtf8(a.entryPathUtf8), BaseNameUtf8(b.entryPathUtf8));
            break;
        case 1:
            cmp = a.archivePath.compare(b.archivePath);
            break;
        case 2:
            cmp = compareText(a.entryPathUtf8, b.entryPathUtf8);
            break;
        case 3: {
            const bool aMissing = a.compressedSize < 0;
            const bool bMissing = b.compressedSize < 0;
            if (aMissing != bMissing) cmp = aMissing ? 1 : -1;
            else cmp = CompareInt64(a.compressedSize, b.compressedSize);
            if (!sortAsc && !aMissing && !bMissing) cmp = -cmp;
            if (cmp == 0) cmp = CompareInt64((*ids)[lhs], (*ids)[rhs]);
            return cmp < 0;
        }
        case 4:
            cmp = CompareUInt64(a.originalSize, b.originalSize);
            break;
        case 5:
            cmp = CompareUInt64(a.modifiedTime, b.modifiedTime);
            break;
        default:
            cmp = CompareInt64((*ids)[lhs], (*ids)[rhs]);
            break;
        }
        if (!sortAsc) cmp = -cmp;
        if (cmp == 0) cmp = CompareInt64((*ids)[lhs], (*ids)[rhs]);
        return cmp < 0;
    });

    std::vector<StoreEntryId> sortedIds;
    sortedIds.reserve(ids->size());
    for (size_t index : order) sortedIds.push_back((*ids)[index]);
    *ids = std::move(sortedIds);
}

class SQLiteIndexStore final : public IndexStore {
public:
    bool Open(const std::wstring& path, std::wstring* err) override { return db_.Open(path, err); }
    void Close() override { db_.Close(); }
    bool IsOpen() const override { return db_.IsOpen(); }
    void SetBusyTimeout(int ms) override { db_.SetBusyTimeout(ms); }
    bool OpenOrCreate(const std::wstring& path, std::wstring* err) override { return Open(path, err) && EnsureSchema(err, true); }
    bool BeginWrite(std::wstring* err, bool bulk = false) override { (void)err; bulkMode_ = bulk; return db_.BeginTransaction(); }
    bool CommitWrite(std::wstring* err) override {
        if (!db_.CommitTransaction()) { bulkMode_ = false; return false; }
        if (bulkMode_) { db_.RebuildFtsIndex(err); }
        bulkMode_ = false;
        return true;
    }
    bool RollbackWrite() override { bulkMode_ = false; return db_.RollbackTransaction(); }

    bool EnsureSchema(std::wstring* err, bool includeConfigs) override
    {
        if (!db_.CreateArchivesTable(err)) return false;
        if (!db_.CreateEntriesTable(err)) return false;
        if (includeConfigs && !db_.CreateConfigsTable(err)) return false;
        return true;
    }

    bool SaveJournalUsn(wchar_t driveLetter, int64_t journalId, USN nextUsn, std::wstring* err) override { return db_.SaveJournalUsn(driveLetter, journalId, nextUsn, err); }
    bool GetJournalUsn(wchar_t driveLetter, int64_t* outJournalId, USN* outNextUsn) override { return db_.GetJournalUsn(driveLetter, outJournalId, outNextUsn); }
    bool SaveConfigValue(const std::string& key, const std::string& value, std::wstring* err) override { return db_.SaveConfigValue(key, value, err); }
    bool GetConfigValue(const std::string& key, std::string* outValue) override { return db_.GetConfigValue(key, outValue); }
    bool UpsertArchive(const ArchiveFile_t& archiveFile) override { return db_.InsertOrUpdateArchive(archiveFile); }
    bool UpsertArchives(const std::vector<ArchiveFile_t>& files, std::wstring* err) override { return db_.InsertArchivesBatch(files, err); }
    bool UpsertArchives(const std::vector<ArchiveFile_t>& files, std::vector<int64_t>* outIds, std::wstring* err) override
    {
        if (!db_.InsertArchivesBatch(files, err)) return false;
        if (outIds) {
            outIds->clear();
            outIds->reserve(files.size());
            for (const auto& file : files) outIds->push_back(db_.GetArchiveIdByPath(file.filePath));
        }
        return true;
    }
    bool GetArchiveByRef(wchar_t driveLetter, uint64_t fileRefNumber, ArchiveFile_t* out) override { return db_.QueryArchiveByRefNumber(driveLetter, fileRefNumber, out); }
    bool QueryArchives(const std::wstring& filter, std::vector<ArchiveFile_t>* out, std::wstring* err) override { return db_.QueryArchives(filter, out, err); }
    bool DeleteArchiveByRef(wchar_t driveLetter, uint64_t fileRefNumber) override { return db_.DeleteArchiveByRefNumber(driveLetter, fileRefNumber); }
    int64_t GetArchiveCount() override { return db_.GetArchiveCount(); }
    int64_t GetArchiveIdByPath(const std::wstring& filePath) override { return db_.GetArchiveIdByPath(filePath); }
    bool DeleteEntriesByArchivePath(const std::wstring& archivePath, std::wstring* err) override { return db_.DeleteEntriesByArchivePath(archivePath, err); }
    bool DeleteEntriesByArchiveId(int64_t archiveId, std::wstring* err) override { return db_.DeleteEntriesByArchiveId(archiveId, err); }
    bool InsertEntries(const std::vector<ArchiveEntry_t>& entries, std::wstring* err) override { return db_.InsertEntriesBatch(entries, err); }
    bool BeginReplaceArchiveEntriesByArchiveId(int64_t archiveId, std::wstring* err) override
    {
        return DeleteEntriesByArchiveId(archiveId, err);
    }
    bool AppendArchiveEntriesByArchiveId(int64_t archiveId, const std::vector<ArchiveEntry_t>& entries, std::wstring* err) override
    {
        std::vector<ArchiveEntry_t> normalized;
        normalized.reserve(entries.size());
        for (auto entry : entries) {
            entry.archiveId = archiveId;
            normalized.push_back(std::move(entry));
        }
        return InsertEntries(normalized, err);
    }
    bool FinishReplaceArchiveEntriesByArchiveId(int64_t archiveId, std::wstring* err) override
    {
        (void)archiveId;
        (void)err;
        return true;
    }
    bool AbortReplaceArchiveEntriesByArchiveId(int64_t archiveId, std::wstring* err) override
    {
        return DeleteEntriesByArchiveId(archiveId, err);
    }
    bool ReplaceArchiveEntriesByArchiveId(int64_t archiveId, const std::vector<ArchiveEntry_t>& entries, std::wstring* err) override
    {
        if (!db_.DeleteEntriesByArchiveId(archiveId, err)) return false;
        return db_.InsertEntriesBatch(entries, err);
    }
    bool ReplaceArchiveEntriesByRef(wchar_t driveLetter, uint64_t fileRefNumber, const std::vector<ArchiveEntry_t>& entries, std::wstring* err) override
    {
        ArchiveFile_t archive;
        if (!GetArchiveByRef(driveLetter, fileRefNumber, &archive)) return false;
        return ReplaceArchiveEntriesByArchiveId(GetArchiveIdByPath(archive.filePath), entries, err);
    }
    bool QueryEntryIds(const std::wstring& filter, int sortColumn, bool sortAsc, std::vector<StoreEntryId>* outIds, std::wstring* err) override { return db_.QueryEntryIds(filter, sortColumn, sortAsc, outIds, err); }
    bool QueryEntriesPage(const EntryQuerySpec& query, EntryQueryPage* out, std::wstring* err) override
    {
        if (!out) { SetErr(err, L"out is null"); return false; }
        std::vector<StoreEntryId> ids;
        if (!db_.QueryEntryIds(query.filter, query.sortColumn, query.sortAscending, &ids, err)) return false;
        out->totalCount = static_cast<int64_t>(ids.size());
        size_t offset = query.offset < ids.size() ? query.offset : ids.size();
        size_t limit = query.limit ? query.limit : ids.size() - offset;
        if (limit > ids.size() - offset) limit = ids.size() - offset;
        out->ids.assign(ids.begin() + offset, ids.begin() + offset + limit);
        return true;
    }
    bool QueryEntryById(StoreEntryId rowId, ArchiveEntry_t* out) override { return db_.QueryEntryById(rowId, out); }
    bool GetEntriesBatch(const std::vector<StoreEntryId>& ids, std::vector<ArchiveEntry_t>* out, std::wstring* err) override
    {
        if (!out) { SetErr(err, L"out is null"); return false; }
        out->clear();
        for (StoreEntryId id : ids) {
            ArchiveEntry_t entry;
            if (!db_.QueryEntryById(id, &entry)) { SetErr(err, L"entry lookup failed"); return false; }
            out->push_back(std::move(entry));
        }
        return true;
    }
    int64_t GetEntryCount(const std::wstring& filter) override { return db_.GetEntryCount(filter); }
    bool Compact() override { return db_.Vacuum(); }

private:
    Database db_;
    bool bulkMode_ = false;
};

class EzdbIndexStore final : public IndexStore {
public:
    ~EzdbIndexStore() override { Close(); }

    bool Open(const std::wstring& path, std::wstring* err) override
    {
        Close();
        path_ = path;
        int rc = ezdb_open(WideToUtf8(path).c_str(), &db_);
        if (rc != 0) {
            SetErr(err, Utf8ToWString(ezdb_error_message(rc)));
            db_ = nullptr;
            return false;
        }
        return true;
    }

    bool OpenOrCreate(const std::wstring& path, std::wstring* err) override
    {
        if (Open(path, err)) return true;
        if (!BuildEmptyEzdb(path, err)) return false;
        return Open(path, err);
    }

    void Close() override
    {
        if (db_) ezdb_close(db_);
        db_ = nullptr;
        txnActive_ = false;
        mutableLoaded_ = false;
        archives_.clear();
        entries_.clear();
    }

    bool IsOpen() const override { return db_ != nullptr; }
    void SetBusyTimeout(int ms) override { (void)ms; }
    bool EnsureSchema(std::wstring* err, bool includeConfigs) override { (void)err; (void)includeConfigs; return true; }

    bool BeginWrite(std::wstring* err, bool bulk = false) override
    {
        if (txnActive_) { SetErr(err, L"write transaction already active"); return false; }
        if (!db_) { SetErr(err, L"store not open"); return false; }
        int rc = ezdb_begin_write(db_, bulk ? EZDB_WRITE_BULK : 0u);
        if (rc != 0) { SetErr(err, Utf8ToWString(ezdb_error_message(rc))); return false; }
        txnActive_ = true;
        dirty_ = false;
        return true;
    }

    bool CommitWrite(std::wstring* err) override
    {
        if (!txnActive_) { SetErr(err, L"write transaction not active"); return false; }
        int rc = ezdb_commit_write(db_);
        bool ok = rc == 0;
        if (!ok) {
            SetErr(err, Utf8ToWString(ezdb_error_message(rc)));
        } else {
            ok = ApplyPendingMeta(err);
        }
        txnActive_ = false;
        mutableLoaded_ = false;
        archives_.clear();
        entries_.clear();
        dirty_ = false;
        pendingMeta_.clear();
        return ok;
    }

    bool RollbackWrite() override
    {
        if (db_ && txnActive_) (void)ezdb_rollback_write(db_);
        txnActive_ = false;
        mutableLoaded_ = false;
        archives_.clear();
        entries_.clear();
        dirty_ = false;
        pendingMeta_.clear();
        return true;
    }

    bool SaveJournalUsn(wchar_t driveLetter, int64_t journalId, USN nextUsn, std::wstring* err) override
    {
        std::string drive(1, static_cast<char>(driveLetter));
        return PutMeta("journal_id_" + drive, std::to_string(journalId), err) &&
               PutMeta("next_usn_" + drive, std::to_string(static_cast<int64_t>(nextUsn)), err);
    }

    bool GetJournalUsn(wchar_t driveLetter, int64_t* outJournalId, USN* outNextUsn) override
    {
        if (!outJournalId || !outNextUsn) return false;
        std::string drive(1, static_cast<char>(driveLetter));
        std::string journal;
        std::string next;
        if (!GetMeta("journal_id_" + drive, &journal) || !GetMeta("next_usn_" + drive, &next)) return false;
        *outJournalId = std::strtoll(journal.c_str(), nullptr, 10);
        *outNextUsn = static_cast<USN>(std::strtoll(next.c_str(), nullptr, 10));
        return true;
    }

    bool SaveConfigValue(const std::string& key, const std::string& value, std::wstring* err) override { return PutMeta("config_" + key, value, err); }
    bool GetConfigValue(const std::string& key, std::string* outValue) override { return GetMeta("config_" + key, outValue); }

    bool UpsertArchive(const ArchiveFile_t& archiveFile) override
    {
        std::wstring err;
        std::vector<ArchiveFile_t> one{archiveFile};
        return UpsertArchives(one, nullptr, &err);
    }

    bool UpsertArchives(const std::vector<ArchiveFile_t>& files, std::wstring* err) override { return UpsertArchives(files, nullptr, err); }

    bool UpsertArchives(const std::vector<ArchiveFile_t>& files, std::vector<int64_t>* outIds, std::wstring* err) override
    {
        if (!LoadMutable(err)) return false;
        if (outIds) outIds->clear();
        for (const auto& file : files) {
            int64_t id = FindArchiveByRefOrPath(file);
            if (id < 0) {
                id = static_cast<int64_t>(archives_.size());
                archives_.push_back(file);
            } else {
                archives_[static_cast<size_t>(id)] = file;
            }
            if (outIds) outIds->push_back(id);
        }
        dirty_ = true;
        return RebuildAndUnload(err);
    }

    bool GetArchiveByRef(wchar_t driveLetter, uint64_t fileRefNumber, ArchiveFile_t* out) override
    {
        if (!db_ || !out) return false;
        EzdbArchiveResult result{};
        int rc = ezdb_get_archive_by_ref(db_, static_cast<char>(driveLetter), fileRefNumber, &result);
        if (rc != 0) return false;
        *out = ArchiveFromEzdb(result);
        ezdb_free_archive_result(&result);
        return true;
    }

    bool QueryArchives(const std::wstring& filter, std::vector<ArchiveFile_t>* out, std::wstring* err) override
    {
        if (!db_ || !out) { SetErr(err, L"store not open"); return false; }
        out->clear();
        const uint32_t count = ezdb_count(db_);
        const std::wstring lowered = ToLower(filter);
        for (uint32_t i = 0; i < count; ++i) {
            EzdbArchiveResult result{};
            if (ezdb_get_archive(db_, i, &result) != 0) continue;
            ArchiveFile_t archive = ArchiveFromEzdb(result);
            ezdb_free_archive_result(&result);
            if (lowered.empty() || ToLower(archive.filePath).find(lowered) != std::wstring::npos) {
                out->push_back(std::move(archive));
            }
        }
        return true;
    }

    bool DeleteArchiveByRef(wchar_t driveLetter, uint64_t fileRefNumber) override
    {
        std::wstring err;
        if (!LoadMutable(&err)) return false;
        int64_t id = FindArchiveByRef(driveLetter, fileRefNumber);
        if (id < 0) return false;
        entries_.erase(std::remove_if(entries_.begin(), entries_.end(), [&](const ArchiveEntry_t& e) {
            return e.archiveId == id;
        }), entries_.end());
        archives_.erase(archives_.begin() + id);
        for (auto& entry : entries_) {
            if (entry.archiveId > id) --entry.archiveId;
        }
        dirty_ = true;
        return RebuildAndUnload(&err);
    }

    int64_t GetArchiveCount() override { return db_ ? ezdb_active_count(db_) : 0; }

    int64_t GetArchiveIdByPath(const std::wstring& filePath) override
    {
        if (!db_) return -1;
        const uint32_t count = ezdb_count(db_);
        for (uint32_t i = 0; i < count; ++i) {
            EzdbArchiveResult result{};
            if (ezdb_get_archive(db_, i, &result) != 0) continue;
            ArchiveFile_t archive = ArchiveFromEzdb(result);
            ezdb_free_archive_result(&result);
            if (archive.filePath == filePath) return static_cast<int64_t>(i);
        }
        return -1;
    }

    bool DeleteEntriesByArchivePath(const std::wstring& archivePath, std::wstring* err) override
    {
        if (!LoadMutable(err)) return false;
        int64_t archiveId = -1;
        for (size_t i = 0; i < archives_.size(); ++i) {
            if (archives_[i].filePath == archivePath) { archiveId = static_cast<int64_t>(i); break; }
        }
        if (archiveId < 0) return true;
        return DeleteEntriesByArchiveId(archiveId, err);
    }

    bool DeleteEntriesByArchiveId(int64_t archiveId, std::wstring* err) override
    {
        if (!LoadMutable(err)) return false;
        entries_.erase(std::remove_if(entries_.begin(), entries_.end(), [&](const ArchiveEntry_t& e) {
            return e.archiveId == archiveId;
        }), entries_.end());
        dirty_ = true;
        return RebuildAndUnload(err);
    }

    bool InsertEntries(const std::vector<ArchiveEntry_t>& entries, std::wstring* err) override
    {
        if (!LoadMutable(err)) return false;
        entries_.insert(entries_.end(), entries.begin(), entries.end());
        dirty_ = true;
        return RebuildAndUnload(err);
    }

    bool BeginReplaceArchiveEntriesByArchiveId(int64_t archiveId, std::wstring* err) override
    {
        if (!db_ || archiveId < 0 || archiveId > UINT32_MAX) { SetErr(err, L"store not open or invalid archive id"); return false; }
        int rc = ezdb_begin_replace_archive_entries(db_, static_cast<uint32_t>(archiveId));
        if (rc != 0) SetErr(err, Utf8ToWString(ezdb_error_message(rc)));
        return rc == 0;
    }

    bool AppendArchiveEntriesByArchiveId(int64_t archiveId,
                                         const std::vector<ArchiveEntry_t>& entries,
                                         std::wstring* err) override
    {
        if (!db_ || archiveId < 0 || archiveId > UINT32_MAX) { SetErr(err, L"store not open or invalid archive id"); return false; }
        std::vector<EzdbEntryRecord> records;
        records.reserve(entries.size());
        for (const auto& entry : entries) {
            EzdbEntryRecord record{};
            record.archive_id = static_cast<uint32_t>(archiveId);
            record.entry_path = entry.entryPathUtf8.c_str();
            if (!entry.entryRawPath.empty()) {
                record.entry_raw_path = entry.entryRawPath.data();
                record.entry_raw_path_len = static_cast<uint32_t>(entry.entryRawPath.size());
            }
            record.compressed_size = entry.compressedSize;
            record.original_size = entry.originalSize;
            record.modified_time = entry.modifiedTime;
            records.push_back(record);
        }
        int rc = ezdb_append_archive_entries(db_, static_cast<uint32_t>(archiveId), records.data(), static_cast<uint32_t>(records.size()));
        if (rc != 0) SetErr(err, Utf8ToWString(ezdb_error_message(rc)));
        return rc == 0;
    }

    bool FinishReplaceArchiveEntriesByArchiveId(int64_t archiveId, std::wstring* err) override
    {
        if (!db_ || archiveId < 0 || archiveId > UINT32_MAX) { SetErr(err, L"store not open or invalid archive id"); return false; }
        int rc = ezdb_finish_replace_archive_entries(db_, static_cast<uint32_t>(archiveId));
        if (rc != 0) SetErr(err, Utf8ToWString(ezdb_error_message(rc)));
        return rc == 0;
    }

    bool AbortReplaceArchiveEntriesByArchiveId(int64_t archiveId, std::wstring* err) override
    {
        if (!db_ || archiveId < 0 || archiveId > UINT32_MAX) { SetErr(err, L"store not open or invalid archive id"); return false; }
        int rc = ezdb_abort_replace_archive_entries(db_, static_cast<uint32_t>(archiveId));
        if (rc != 0) SetErr(err, Utf8ToWString(ezdb_error_message(rc)));
        return rc == 0;
    }

    bool ReplaceArchiveEntriesByArchiveId(int64_t archiveId, const std::vector<ArchiveEntry_t>& entries, std::wstring* err) override
    {
        return BeginReplaceArchiveEntriesByArchiveId(archiveId, err) &&
               AppendArchiveEntriesByArchiveId(archiveId, entries, err) &&
               FinishReplaceArchiveEntriesByArchiveId(archiveId, err);
    }

    bool ReplaceArchiveEntriesByRef(wchar_t driveLetter, uint64_t fileRefNumber, const std::vector<ArchiveEntry_t>& entries, std::wstring* err) override
    {
        if (!LoadMutable(err)) return false;
        int64_t archiveId = FindArchiveByRef(driveLetter, fileRefNumber);
        if (archiveId < 0) { SetErr(err, L"archive not found"); return false; }
        return ReplaceArchiveEntriesByArchiveId(archiveId, entries, err);
    }

    bool QueryEntryIds(const std::wstring& filter, int sortColumn, bool sortAsc, std::vector<StoreEntryId>* outIds, std::wstring* err) override
    {
        EntryQuerySpec query;
        query.filter = filter;
        query.scope = EntrySearchScope::Combined;
        query.sortColumn = sortColumn;
        query.sortAscending = sortAsc;
        EntryQueryPage page;
        if (!QueryEntriesPage(query, &page, err)) return false;
        if (outIds) *outIds = std::move(page.ids);
        return true;
    }

    bool QueryEntriesPage(const EntryQuerySpec& query, EntryQueryPage* out, std::wstring* err) override
    {
        if (!db_ || !out) { SetErr(err, L"store not open"); return false; }
        out->ids.clear();
        out->rows.clear();
        std::string keyword = WideToUtf8(query.filter);
        const bool needsSort = query.sortColumn >= 0 || !query.sortAscending;
        EzdbEntryQuery cquery{};
        cquery.keyword = keyword.c_str();
        cquery.scope = ToEzdbScope(query.scope);
        cquery.sort_column = -1;
        cquery.sort_ascending = 1;
        cquery.offset = needsSort ? 0 : query.offset;
        cquery.limit = needsSort ? 0 : query.limit;
        EzdbEntryQueryPage page{};
        int rc = ezdb_query_entries(db_, &cquery, &page);
        if (rc != 0) {
            SetErr(err, Utf8ToWString(ezdb_error_message(rc)));
            return false;
        }
        out->totalCount = static_cast<int64_t>(page.total_count);
        out->ids.reserve(page.returned_count);
        for (uint32_t i = 0; i < page.returned_count; ++i) out->ids.push_back(page.ids[i]);
        ezdb_free_entry_query_page(&page);

        if (needsSort) {
            std::vector<ArchiveEntry_t> rows;
            if (!GetEntriesBatch(out->ids, &rows, err)) return false;
            SortEntriesForQuery(&out->ids, rows, query.sortColumn, query.sortAscending);

            const size_t offset = query.offset < out->ids.size() ? query.offset : out->ids.size();
            const size_t available = out->ids.size() - offset;
            size_t limit = query.limit ? query.limit : available;
            if (limit > available) limit = available;
            std::vector<StoreEntryId> pageIds(out->ids.begin() + offset, out->ids.begin() + offset + limit);
            out->ids = std::move(pageIds);
        }
        return true;
    }

    bool QueryEntryById(StoreEntryId rowId, ArchiveEntry_t* out) override
    {
        if (!db_ || !out || rowId < 0 || rowId > UINT32_MAX) return false;
        EzdbEntryResult result{};
        int rc = ezdb_get_entry(db_, static_cast<uint32_t>(rowId), &result);
        if (rc != 0) return false;
        *out = EntryFromEzdb(result);
        ezdb_free_entry_result(&result);
        return true;
    }

    bool GetEntriesBatch(const std::vector<StoreEntryId>& ids, std::vector<ArchiveEntry_t>* out, std::wstring* err) override
    {
        if (!db_ || !out) { SetErr(err, L"store not open"); return false; }
        out->clear();
        out->reserve(ids.size());
        for (StoreEntryId id : ids) {
            ArchiveEntry_t entry;
            if (!QueryEntryById(id, &entry)) { SetErr(err, L"entry lookup failed"); return false; }
            out->push_back(std::move(entry));
        }
        return true;
    }

    int64_t GetEntryCount(const std::wstring& filter) override
    {
        EntryQuerySpec query;
        query.filter = filter;
        query.limit = 0;
        EntryQueryPage page;
        std::wstring err;
        return QueryEntriesPage(query, &page, &err) ? page.totalCount : 0;
    }

    bool Compact() override
    {
        if (!db_) return false;
        int rc = ezdb_compact(db_);
        return rc == 0;
    }

private:
    uint32_t ToEzdbScope(EntrySearchScope scope) const
    {
        switch (scope) {
        case EntrySearchScope::EntryPath: return EZDB_SEARCH_ENTRY_PATH;
        case EntrySearchScope::ArchivePath: return EZDB_SEARCH_ARCHIVE_PATH;
        case EntrySearchScope::All: return EZDB_SEARCH_ALL;
        case EntrySearchScope::Combined:
        default: return EZDB_SEARCH_COMBINED_PATH;
        }
    }

    bool PutMeta(const std::string& key, const std::string& value, std::wstring* err)
    {
        if (!db_) { SetErr(err, L"store not open"); return false; }
        if (txnActive_) {
            pendingMeta_[key] = value;
            return true;
        }
        int rc = ezdb_put_meta(db_, key.c_str(), value.c_str());
        if (rc != 0) SetErr(err, Utf8ToWString(ezdb_error_message(rc)));
        return rc == 0;
    }

    bool ApplyPendingMeta(std::wstring* err)
    {
        for (const auto& kv : pendingMeta_) {
            int rc = ezdb_put_meta(db_, kv.first.c_str(), kv.second.c_str());
            if (rc != 0) {
                SetErr(err, Utf8ToWString(ezdb_error_message(rc)));
                return false;
            }
        }
        return true;
    }

    bool GetMeta(const std::string& key, std::string* outValue)
    {
        if (!db_ || !outValue) return false;
        char* value = nullptr;
        int rc = ezdb_get_meta(db_, key.c_str(), &value);
        if (rc != 0) return false;
        *outValue = value ? value : "";
        free(value);
        return true;
    }

    int64_t FindArchiveByRef(wchar_t driveLetter, uint64_t fileRefNumber) const
    {
        for (size_t i = 0; i < archives_.size(); ++i) {
            if (!archives_[i].driveLetter.empty() &&
                archives_[i].driveLetter[0] == driveLetter &&
                static_cast<uint64_t>(archives_[i].fileRefNumber) == fileRefNumber) {
                return static_cast<int64_t>(i);
            }
        }
        return -1;
    }

    int64_t FindArchiveByRefOrPath(const ArchiveFile_t& file) const
    {
        if (!file.driveLetter.empty() && file.fileRefNumber) {
            int64_t byRef = FindArchiveByRef(file.driveLetter[0], static_cast<uint64_t>(file.fileRefNumber));
            if (byRef >= 0) return byRef;
        }
        for (size_t i = 0; i < archives_.size(); ++i) {
            if (archives_[i].filePath == file.filePath) return static_cast<int64_t>(i);
        }
        return -1;
    }

    bool LoadMutable(std::wstring* err)
    {
        if (mutableLoaded_) return true;
        if (!db_) { SetErr(err, L"store not open"); return false; }
        archives_.clear();
        entries_.clear();
        std::vector<int64_t> archiveIdMap(ezdb_count(db_), -1);
        for (uint32_t i = 0; i < ezdb_count(db_); ++i) {
            EzdbArchiveResult result{};
            if (ezdb_get_archive(db_, i, &result) != 0) continue;
            archiveIdMap[i] = static_cast<int64_t>(archives_.size());
            archives_.push_back(ArchiveFromEzdb(result));
            ezdb_free_archive_result(&result);
        }
        for (uint32_t i = 0; i < ezdb_entry_count(db_); ++i) {
            EzdbEntryResult result{};
            if (ezdb_get_entry(db_, i, &result) != 0) continue;
            if (result.archive_id >= archiveIdMap.size() || archiveIdMap[result.archive_id] < 0) {
                ezdb_free_entry_result(&result);
                continue;
            }
            ArchiveEntry_t entry = EntryFromEzdb(result);
            entry.archiveId = archiveIdMap[result.archive_id];
            entries_.push_back(std::move(entry));
            ezdb_free_entry_result(&result);
        }
        mutableLoaded_ = true;
        return true;
    }

    bool RebuildAndUnload(std::wstring* err)
    {
        bool ok = Rebuild(err);
        mutableLoaded_ = false;
        archives_.clear();
        entries_.clear();
        dirty_ = false;
        return ok;
    }

    bool Rebuild(std::wstring* err)
    {
        std::vector<std::string> archivePaths;
        std::vector<EzdbArchiveRecord> archives;
        archivePaths.reserve(archives_.size());
        archives.reserve(archives_.size());
        for (const auto& archive : archives_) {
            archivePaths.emplace_back();
            archives.push_back(MakeEzdbArchiveRecord(archive, &archivePaths.back()));
        }

        std::vector<EzdbEntryRecord> entries;
        entries.reserve(entries_.size());
        for (const auto& entry : entries_) {
            if (entry.archiveId < 0 || static_cast<size_t>(entry.archiveId) >= archives.size()) continue;
            EzdbEntryRecord record{};
            record.archive_id = static_cast<uint32_t>(entry.archiveId);
            record.entry_path = entry.entryPathUtf8.c_str();
            if (!entry.entryRawPath.empty()) {
                record.entry_raw_path = entry.entryRawPath.data();
                record.entry_raw_path_len = static_cast<uint32_t>(entry.entryRawPath.size());
            }
            record.compressed_size = entry.compressedSize;
            record.original_size = entry.originalSize;
            record.modified_time = entry.modifiedTime;
            entries.push_back(record);
        }

        const std::wstring tmp = path_ + L".tmp";
        DeleteFileW(tmp.c_str());
        int rc = ezdb_build_snapshot(archives.data(), static_cast<uint32_t>(archives.size()),
                                     entries.data(), static_cast<uint32_t>(entries.size()),
                                     WideToUtf8(tmp).c_str());
        if (rc != 0) {
            SetErr(err, Utf8ToWString(ezdb_error_message(rc)));
            DeleteFileW(tmp.c_str());
            return false;
        }
        if (db_) {
            ezdb_close(db_);
            db_ = nullptr;
        }
        if (!MoveReplace(tmp, path_, err)) {
            return false;
        }
        return Open(path_, err);
    }

    std::wstring path_;
    Ezdb* db_ = nullptr;
    bool txnActive_ = false;
    bool mutableLoaded_ = false;
    bool dirty_ = false;
    std::unordered_map<std::string, std::string> pendingMeta_;
    std::vector<ArchiveFile_t> archives_;
    std::vector<ArchiveEntry_t> entries_;
};

} // namespace

std::unique_ptr<IndexStore> CreateSQLiteIndexStore()
{
    return std::make_unique<SQLiteIndexStore>();
}

std::unique_ptr<IndexStore> CreateEzdbIndexStore()
{
    return std::make_unique<EzdbIndexStore>();
}

std::unique_ptr<IndexStore> CreateIndexStore()
{
    return CreateEzdbIndexStore();
}
