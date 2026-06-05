#include "../src/index_store.h"

#include <windows.h>

#include <iostream>

static std::wstring MakeTempDbPath(const wchar_t* name)
{
    wchar_t dir[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, dir);
    return std::wstring(dir) + name;
}

static int Fail(const char* message)
{
    std::cerr << message << std::endl;
    return 1;
}

static int RunStoreCase(const wchar_t* name, std::unique_ptr<IndexStore> store)
{
    const std::wstring dbPath = MakeTempDbPath(name);
    DeleteFileW(dbPath.c_str());
    DeleteFileW((dbPath + L"-wal").c_str());
    DeleteFileW((dbPath + L"-shm").c_str());
    DeleteFileW((dbPath + L".meta").c_str());

    std::wstring err;
    if (!store->OpenOrCreate(dbPath, &err)) return Fail("open failed");

    ArchiveFile_t archive;
    archive.driveLetter = L"T";
    archive.filePath = L"T:\\EveryZipTest\\sample.zip";
    archive.fileSize = 1234;
    archive.modifiedTime = 5678;
    archive.fileRefNumber = 42;
    archive.usn = 99;
    if (!store->UpsertArchive(archive)) return Fail("archive upsert failed");

    ArchiveFile_t loaded;
    if (!store->GetArchiveByRef(L'T', archive.fileRefNumber, &loaded)) return Fail("archive ref lookup failed");
    if (loaded.filePath != archive.filePath) return Fail("archive path mismatch");

    const int64_t archiveId = store->GetArchiveIdByPath(archive.filePath);
    if (archiveId == kInvalidStoreEntryId) return Fail("archive id lookup failed");

    ArchiveEntry_t entry;
    entry.archiveId = archiveId;
    entry.entryPathUtf8 = "folder/file.txt";
    entry.compressedSize = 10;
    entry.originalSize = 20;
    entry.modifiedTime = 30;
    std::vector<ArchiveEntry_t> entries{ entry };
    if (!store->ReplaceArchiveEntriesByArchiveId(archiveId, entries, &err)) return Fail("entry replace failed");

    std::vector<StoreEntryId> ids;
    if (!store->QueryEntryIds(L"file", -1, true, &ids, &err)) return Fail("entry id query failed");
    if (ids.size() != 1 || ids[0] == kInvalidStoreEntryId) return Fail("entry id result mismatch");

    ArchiveEntry_t loadedEntry;
    if (!store->QueryEntryById(ids[0], &loadedEntry)) return Fail("entry lookup failed");
    if (loadedEntry.archivePath != archive.filePath || loadedEntry.entryPathUtf8 != entry.entryPathUtf8) {
        return Fail("entry data mismatch");
    }

    EntryQuerySpec query;
    query.filter = L"file";
    query.scope = EntrySearchScope::Combined;
    query.limit = 10;
    EntryQueryPage page;
    if (!store->QueryEntriesPage(query, &page, &err)) return Fail("entry page query failed");
    if (page.totalCount != 1 || page.ids.size() != 1) return Fail("entry page result mismatch");

    std::vector<ArchiveEntry_t> batch;
    if (!store->GetEntriesBatch(page.ids, &batch, &err)) return Fail("entry batch failed");
    if (batch.size() != 1 || batch[0].entryPathUtf8 != entry.entryPathUtf8) return Fail("entry batch mismatch");

    if (!store->SaveConfigValue("test_key", "test_value", &err)) return Fail("config save failed");
    std::string value;
    if (!store->GetConfigValue("test_key", &value) || value != "test_value") return Fail("config load failed");

    if (!store->SaveJournalUsn(L'T', 123, 456, &err)) return Fail("journal save failed");
    int64_t journalId = 0;
    USN nextUsn = 0;
    if (!store->GetJournalUsn(L'T', &journalId, &nextUsn)) return Fail("journal load failed");
    if (journalId != 123 || nextUsn != 456) return Fail("journal value mismatch");

    if (store->GetArchiveCount() != 1) return Fail("archive count mismatch");
    if (store->GetEntryCount(L"file") != 1) return Fail("entry count mismatch");

    {
        ArchiveEntry_t txnEntryA = entry;
        txnEntryA.entryPathUtf8 = "folder/txn_a.txt";
        ArchiveEntry_t txnEntryB = entry;
        txnEntryB.entryPathUtf8 = "folder/txn_b.txt";
        std::vector<ArchiveEntry_t> txnBatchA{ txnEntryA };
        std::vector<ArchiveEntry_t> txnBatchB{ txnEntryB };
        if (!store->BeginWrite(&err)) return Fail("entry transaction begin failed");
        if (!store->BeginReplaceArchiveEntriesByArchiveId(archiveId, &err)) return Fail("entry transaction replace begin failed");
        if (!store->AppendArchiveEntriesByArchiveId(archiveId, txnBatchA, &err)) return Fail("entry transaction append A failed");
        if (!store->AppendArchiveEntriesByArchiveId(archiveId, txnBatchB, &err)) return Fail("entry transaction append B failed");
        if (!store->FinishReplaceArchiveEntriesByArchiveId(archiveId, &err)) return Fail("entry transaction replace finish failed");
        if (!store->CommitWrite(&err)) return Fail("entry transaction commit failed");
        if (store->GetEntryCount(L"txn_") != 2) return Fail("entry transaction count mismatch");
        if (store->GetEntryCount(L"file.txt") != 0) return Fail("entry transaction stale entry still searchable");
    }

    if (std::wstring(name).find(L".ezdb") != std::wstring::npos) {
        store->Close();
        if (!store->Open(dbPath, &err)) return Fail("ezdb reopen after entry transaction failed");
        if (store->GetEntryCount(L"txn_") != 2) return Fail("ezdb reopen entry transaction count mismatch");
        if (store->GetEntryCount(L"file.txt") != 0) return Fail("ezdb reopen stale entry still searchable");
    }

    if (!store->Compact()) return Fail("compact failed");

    store->Close();
    DeleteFileW(dbPath.c_str());
    DeleteFileW((dbPath + L"-wal").c_str());
    DeleteFileW((dbPath + L"-shm").c_str());
    DeleteFileW((dbPath + L".meta").c_str());
    return 0;
}

static int ExpectEzdbScopeResult(IndexStore* store,
                                 EntrySearchScope scope,
                                 const std::wstring& filter,
                                 const std::wstring& expectedArchivePath,
                                 const std::string& expectedEntryPath)
{
    EntryQuerySpec query;
    query.filter = filter;
    query.scope = scope;
    query.limit = 10;
    EntryQueryPage page;
    std::wstring err;
    if (!store->QueryEntriesPage(query, &page, &err)) return Fail("ezdb scope query failed");
    if (page.totalCount != 1 || page.ids.size() != 1) return Fail("ezdb scope query count mismatch");

    ArchiveEntry_t loadedEntry;
    if (!store->QueryEntryById(page.ids[0], &loadedEntry)) return Fail("ezdb scope entry lookup failed");
    if (loadedEntry.archivePath != expectedArchivePath || loadedEntry.entryPathUtf8 != expectedEntryPath) {
        return Fail("ezdb scope entry mismatch");
    }
    return 0;
}

static int RunEzdbEntryScopeCase()
{
    const std::wstring dbPath = MakeTempDbPath(L"everyzip_entry_scope_test.ezdb");
    DeleteFileW(dbPath.c_str());
    DeleteFileW((dbPath + L".meta").c_str());

    auto store = CreateEzdbIndexStore();
    std::wstring err;
    if (!store->OpenOrCreate(dbPath, &err)) return Fail("ezdb scope open failed");

    ArchiveFile_t archiveA;
    archiveA.driveLetter = L'S';
    archiveA.filePath = L"S:\\EveryZipTest\\scope_archive.zip";
    archiveA.fileSize = 100;
    archiveA.modifiedTime = 200;
    archiveA.fileRefNumber = 1001;
    archiveA.usn = 2001;
    if (!store->UpsertArchive(archiveA)) return Fail("ezdb scope archive A upsert failed");
    const int64_t archiveAId = store->GetArchiveIdByPath(archiveA.filePath);
    if (archiveAId == kInvalidStoreEntryId) return Fail("ezdb scope archive A id failed");

    ArchiveEntry_t entryA;
    entryA.archiveId = archiveAId;
    entryA.entryPathUtf8 = "folder/archive_child.txt";
    entryA.compressedSize = 10;
    entryA.originalSize = 20;
    entryA.modifiedTime = 30;
    if (!store->ReplaceArchiveEntriesByArchiveId(archiveAId, { entryA }, &err)) return Fail("ezdb scope entry A write failed");

    ArchiveFile_t archiveB;
    archiveB.driveLetter = L'S';
    archiveB.filePath = L"S:\\EveryZipTest\\plain.zip";
    archiveB.fileSize = 300;
    archiveB.modifiedTime = 400;
    archiveB.fileRefNumber = 1002;
    archiveB.usn = 2002;
    if (!store->UpsertArchive(archiveB)) return Fail("ezdb scope archive B upsert failed");
    const int64_t archiveBId = store->GetArchiveIdByPath(archiveB.filePath);
    if (archiveBId == kInvalidStoreEntryId) return Fail("ezdb scope archive B id failed");

    ArchiveEntry_t entryB;
    entryB.archiveId = archiveBId;
    entryB.entryPathUtf8 = "folder/scope_archive_entry.txt";
    entryB.compressedSize = 11;
    entryB.originalSize = 21;
    entryB.modifiedTime = 31;
    if (!store->ReplaceArchiveEntriesByArchiveId(archiveBId, { entryB }, &err)) return Fail("ezdb scope entry B write failed");

    if (ExpectEzdbScopeResult(store.get(), EntrySearchScope::ArchivePath, L"scope_archive", archiveA.filePath, entryA.entryPathUtf8) != 0) return 1;
    if (ExpectEzdbScopeResult(store.get(), EntrySearchScope::EntryPath, L"scope_archive", archiveB.filePath, entryB.entryPathUtf8) != 0) return 1;
    if (ExpectEzdbScopeResult(store.get(), EntrySearchScope::Combined, L"scope_archive archive_child", archiveA.filePath, entryA.entryPathUtf8) != 0) return 1;

    store->Close();
    DeleteFileW(dbPath.c_str());
    DeleteFileW((dbPath + L".meta").c_str());
    return 0;
}

static int RunSQLiteImportCase()
{
    const std::wstring sqlitePath = MakeTempDbPath(L"everyzip_index_store_import.db");
    const std::wstring ezdbPath = MakeTempDbPath(L"everyzip_index_store_import.ezdb");
    DeleteFileW(sqlitePath.c_str());
    DeleteFileW(ezdbPath.c_str());
    DeleteFileW((ezdbPath + L".meta").c_str());
    DeleteFileW((ezdbPath + L".tmp.archives.tmp.tsv").c_str());

    {
        auto source = CreateSQLiteIndexStore();
        std::wstring err;
        if (!source->OpenOrCreate(sqlitePath, &err)) return Fail("sqlite import source open failed");

        ArchiveFile_t archive;
        archive.driveLetter = L"I";
        archive.filePath = L"I:\\EveryZipTest\\import.zip";
        archive.fileSize = 200;
        archive.modifiedTime = 300;
        archive.fileRefNumber = 777;
        archive.usn = 888;
        if (!source->UpsertArchive(archive)) return Fail("sqlite import archive upsert failed");

        const int64_t archiveId = source->GetArchiveIdByPath(archive.filePath);
        ArchiveEntry_t entry;
        entry.archiveId = archiveId;
        entry.entryPathUtf8 = "imported/file.txt";
        entry.compressedSize = 11;
        entry.originalSize = 22;
        entry.modifiedTime = 33;
        if (!source->ReplaceArchiveEntriesByArchiveId(archiveId, { entry }, &err)) return Fail("sqlite import entry write failed");
        if (!source->SaveConfigValue("import_key", "import_value", &err)) return Fail("sqlite import config save failed");
        if (!source->SaveJournalUsn(L'I', 901, 902, &err)) return Fail("sqlite import journal save failed");
    }

    {
        auto imported = CreateEzdbIndexStore();
        std::wstring err;
        if (!imported->OpenOrCreate(ezdbPath, &err)) return Fail("ezdb import open failed");
        if (imported->GetArchiveCount() != 1) return Fail("ezdb import archive count mismatch");
        if (imported->GetEntryCount(L"imported") != 1) return Fail("ezdb import entry count mismatch");
        if (GetFileAttributesW((ezdbPath + L".tmp.archives.tmp.tsv").c_str()) != INVALID_FILE_ATTRIBUTES) {
            return Fail("ezdb import leaked archive temp tsv");
        }

        std::string value;
        if (!imported->GetConfigValue("import_key", &value) || value != "import_value") {
            return Fail("ezdb import config mismatch");
        }
        int64_t journalId = 0;
        USN nextUsn = 0;
        if (!imported->GetJournalUsn(L'I', &journalId, &nextUsn) || journalId != 901 || nextUsn != 902) {
            return Fail("ezdb import journal mismatch");
        }
    }

    DeleteFileW(sqlitePath.c_str());
    DeleteFileW((sqlitePath + L"-wal").c_str());
    DeleteFileW((sqlitePath + L"-shm").c_str());
    DeleteFileW(ezdbPath.c_str());
    DeleteFileW((ezdbPath + L".meta").c_str());
    DeleteFileW((ezdbPath + L".tmp.archives.tmp.tsv").c_str());
    return 0;
}

int main()
{
    if (RunStoreCase(L"everyzip_index_store_test.db", CreateSQLiteIndexStore()) != 0) return 1;
    if (RunStoreCase(L"everyzip_index_store_test.ezdb", CreateEzdbIndexStore()) != 0) return 1;
    if (RunEzdbEntryScopeCase() != 0) return 1;
    if (RunSQLiteImportCase() != 0) return 1;
    return 0;
}
