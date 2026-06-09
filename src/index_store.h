#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "types.h"

using StoreEntryId = int64_t;
constexpr StoreEntryId kInvalidStoreEntryId = -1;

enum class EntrySearchScope {
    EntryPath,
    ArchivePath,
    Combined,
    All
};

struct EntryQuerySpec {
    std::wstring filter;
    EntrySearchScope scope = EntrySearchScope::Combined;
    int sortColumn = -1;
    bool sortAscending = true;
    uint32_t offset = 0;
    uint32_t limit = 0;
};

struct EntryQueryPage {
    int64_t totalCount = 0;
    std::vector<StoreEntryId> ids;
    std::vector<ArchiveEntry_t> rows;
};

class IndexStore {
public:
    virtual ~IndexStore() = default;

    IndexStore(const IndexStore&) = delete;
    IndexStore& operator=(const IndexStore&) = delete;

    virtual bool Open(const std::wstring& path, std::wstring* err) = 0;
    virtual void Close() = 0;
    virtual bool IsOpen() const = 0;
    virtual void SetBusyTimeout(int ms) = 0;

    virtual bool OpenOrCreate(const std::wstring& path, std::wstring* err) = 0;
    virtual bool EnsureSchema(std::wstring* err, bool includeConfigs) = 0;

    virtual bool BeginWrite(std::wstring* err, bool bulk = false) = 0;
    virtual bool CommitWrite(std::wstring* err) = 0;
    virtual bool RollbackWrite() = 0;

    virtual bool SaveJournalUsn(wchar_t driveLetter, int64_t journalId, USN nextUsn, std::wstring* err) = 0;
    virtual bool GetJournalUsn(wchar_t driveLetter, int64_t* outJournalId, USN* outNextUsn) = 0;
    virtual bool SaveConfigValue(const std::string& key, const std::string& value, std::wstring* err) = 0;
    virtual bool GetConfigValue(const std::string& key, std::string* outValue) = 0;

    virtual bool UpsertArchive(const ArchiveFile_t& archiveFile) = 0;
    virtual bool UpsertArchives(const std::vector<ArchiveFile_t>& files, std::wstring* err) = 0;
    virtual bool UpsertArchives(const std::vector<ArchiveFile_t>& files,
                                std::vector<int64_t>* outIds,
                                std::wstring* err) = 0;
    virtual bool GetArchiveByRef(wchar_t driveLetter, uint64_t fileRefNumber, ArchiveFile_t* out) = 0;
    virtual bool QueryArchives(const std::wstring& filter, std::vector<ArchiveFile_t>* out, std::wstring* err) = 0;
    virtual bool DeleteArchiveByRef(wchar_t driveLetter, uint64_t fileRefNumber) = 0;
    virtual int64_t GetArchiveCount() = 0;
    virtual int64_t GetArchiveIdByPath(const std::wstring& filePath) = 0;

    virtual bool DeleteEntriesByArchivePath(const std::wstring& archivePath, std::wstring* err) = 0;
    virtual bool DeleteEntriesByArchiveId(int64_t archiveId, std::wstring* err) = 0;
    virtual bool InsertEntries(const std::vector<ArchiveEntry_t>& entries, std::wstring* err) = 0;
    virtual bool BeginReplaceArchiveEntriesByArchiveId(int64_t archiveId, std::wstring* err) = 0;
    virtual bool AppendArchiveEntriesByArchiveId(int64_t archiveId,
                                                 const std::vector<ArchiveEntry_t>& entries,
                                                 std::wstring* err) = 0;
    virtual bool FinishReplaceArchiveEntriesByArchiveId(int64_t archiveId, std::wstring* err) = 0;
    virtual bool AbortReplaceArchiveEntriesByArchiveId(int64_t archiveId, std::wstring* err) = 0;
    virtual bool ReplaceArchiveEntriesByArchiveId(int64_t archiveId,
                                                  const std::vector<ArchiveEntry_t>& entries,
                                                  std::wstring* err) = 0;
    virtual bool ReplaceArchiveEntriesByRef(wchar_t driveLetter,
                                            uint64_t fileRefNumber,
                                            const std::vector<ArchiveEntry_t>& entries,
                                            std::wstring* err) = 0;
    virtual bool QueryEntryIds(const std::wstring& filter, int sortColumn, bool sortAsc,
                               std::vector<StoreEntryId>* outIds, std::wstring* err) = 0;
    virtual bool QueryEntriesPage(const EntryQuerySpec& query, EntryQueryPage* out, std::wstring* err) = 0;
    virtual bool QueryEntryById(StoreEntryId rowId, ArchiveEntry_t* out) = 0;
    virtual bool GetEntriesBatch(const std::vector<StoreEntryId>& ids,
                                 std::vector<ArchiveEntry_t>* out,
                                 std::wstring* err) = 0;
    virtual int64_t GetEntryCount(const std::wstring& filter) = 0;

    virtual bool Compact() = 0;

protected:
    IndexStore() = default;
};

std::unique_ptr<IndexStore> CreateSQLiteIndexStore();
std::unique_ptr<IndexStore> CreateEzdbIndexStore();
std::unique_ptr<IndexStore> CreateIndexStore();
