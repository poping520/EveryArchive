#include "indexer.h"

#include "logger.h"
#include "resource.h"
#include "string_utils.h"
#include "parser/archive_parser_factory.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cwctype>
#include <memory>
#include <mutex>
#include <queue>
#include <unordered_map>

Indexer::Indexer() {
    SetArchiveExtensions(archiveExtensions_);
}

Indexer::~Indexer() {
    Stop();
}

static bool IsSevenZipPath(const std::wstring& path) {
    const size_t dotPos = path.find_last_of(L'.');
    if (dotPos == std::wstring::npos) return false;
    std::wstring ext = path.substr(dotPos);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t ch) {
        return (wchar_t)towlower(ch);
    });
    return ext == L".7z";
}

void Indexer::SetDbPath(const std::wstring& dbPath) {
    dbPath_ = dbPath;
}

void Indexer::SetArchiveExtensions(const std::vector<std::wstring>& exts) {
    archiveExtensions_ = exts;
    archiveFormatRules_.clear();
    for (const auto& ext : exts) {
        const std::wstring normalized = UserConfig::NormalizeArchiveExtension(ext);
        if (normalized == L".7z") {
            archiveFormatRules_.push_back({ normalized, L"7z", true, L"custom" });
        } else if (normalized == L".rar") {
            archiveFormatRules_.push_back({ normalized, L"rar", true, L"custom" });
        } else if (!normalized.empty()) {
            archiveFormatRules_.push_back({ normalized, L"zip", true, L"custom" });
        }
    }
}

void Indexer::SetArchiveFormatRules(const std::vector<UserConfig::ArchiveFormatRule>& rules) {
    archiveFormatRules_ = rules;
    archiveExtensions_.clear();
    for (const auto& rule : archiveFormatRules_) {
        if (rule.enabled && !rule.extension.empty()) {
            archiveExtensions_.push_back(rule.extension);
        }
    }
}

void Indexer::SetScanDriveLetters(const std::vector<wchar_t>& drives) {
    scanDriveLetters_ = drives;
}

void Indexer::SetParseThreadCount(uint32_t threads) {
    parseThreadCount_ = threads > 16 ? 16 : threads;
}

static uint32_t ResolveParseThreadCount(uint32_t configured) {
    if (configured > 0) return configured;
    const uint32_t hw = std::thread::hardware_concurrency();
    if (hw == 0) return 2;
    return std::clamp(hw / 2, 2u, 6u);
}

bool Indexer::EnsureDatabaseReady() {
    const DWORD attr = GetFileAttributesW(dbPath_.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) {
        HANDLE h = CreateFileW(dbPath_.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            CloseHandle(h);
        }
    }

    std::wstring err;
    auto store = CreateIndexStore();
    if (!store->OpenOrCreate(dbPath_, &err)) {
        LOG_WARN(L"IndexStore::Open failed: %s", err.c_str());
        return false;
    }
    if (!store->EnsureSchema(&err, false)) {
        LOG_WARN(L"IndexStore::EnsureSchema failed: %s", err.c_str());
        return false;
    }
    return true;
}

bool Indexer::EnablePrivilege(const wchar_t* privilegeName) {
    HANDLE hToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) {
        return false;
    }

    LUID luid{};
    if (!LookupPrivilegeValueW(nullptr, privilegeName, &luid)) {
        CloseHandle(hToken);
        return false;
    }

    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    if (!AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp), nullptr, nullptr)) {
        CloseHandle(hToken);
        return false;
    }

    const DWORD err = GetLastError();
    CloseHandle(hToken);
    return err != ERROR_NOT_ALL_ASSIGNED;
}

static std::wstring GetParserTypeForPath(const std::wstring& path,
                                         const std::vector<UserConfig::ArchiveFormatRule>& rules) {
    const size_t dotPos = path.find_last_of(L'.');
    if (dotPos == std::wstring::npos) return {};
    const std::wstring ext = UserConfig::NormalizeArchiveExtension(path.substr(dotPos));
    for (const auto& rule : rules) {
        if (rule.enabled && rule.extension == ext) {
            return rule.parser;
        }
    }
    return {};
}

struct ParsedArchiveResult {
    ArchiveFile_t archive;
    int64_t archiveId = -1;
    bool ok = false;
    std::vector<ArchiveEntry_t> entries;
};

struct ParsedEntryBatch {
    ArchiveFile_t archive;
    int64_t archiveId = -1;
    bool ok = false;
    bool done = false;
    std::vector<ArchiveEntry_t> entries;
};

static constexpr size_t kParsedEntryBatchSize = 4096;

static ParsedArchiveResult ParseArchiveEntriesOnly(const ArchiveFile_t& a,
                                                   int64_t archiveId,
                                                   const std::wstring& parserType) {
    ParsedArchiveResult result;
    result.archive = a;
    result.archiveId = archiveId;

    if (archiveId < 0) {
        LOG_WARN(L"GetArchiveIdByPath failed for: %s", a.filePath.c_str());
        return result;
    }

    std::unique_ptr<EveryZip::IArchiveParser> parserPtr =
        EveryZip::CreateArchiveParserByType(parserType);
    if (!parserPtr) {
        LOG_WARN(L"No archive parser for: %s (parser=%s)", a.filePath.c_str(), parserType.c_str());
        return result;
    }

    std::string perr;
    if (!parserPtr->Open(a.filePath, &perr)) {
        LOG_WARN(L"ArchiveParser::Open failed (%s): %s",
                 a.filePath.c_str(), Utf8ToWString(perr.c_str()).c_str());
        return result;
    }

    std::vector<ArchiveEntry_t> parsed;
    if (!parserPtr->ListEntries(&parsed, &perr)) {
        LOG_WARN(L"ArchiveParser::ListEntries failed (%s): %s",
                 a.filePath.c_str(), Utf8ToWString(perr.c_str()).c_str());
        parserPtr->Close();
        return result;
    }
    parserPtr->Close();

    result.entries.reserve(parsed.size());
    for (const auto& e : parsed) {
        if (e.isDirectory) continue;
        if (e.entryPathUtf8.empty()) {
            LOG_WARN(L"Skip archive entry with empty UTF-8 path: %s", a.filePath.c_str());
            continue;
        }

        ArchiveEntry_t out;
        out.archiveId = archiveId;
        out.entryPathUtf8 = e.entryPathUtf8;
        out.entryRawPath = e.entryRawPath;
        out.compressedSize = e.compressedSize;
        out.originalSize = e.originalSize;
        out.modifiedTime = e.modifiedTime;
        result.entries.push_back(std::move(out));
    }
    result.ok = true;
    return result;
}

void Indexer::ParseAndStoreArchive(IndexStore& store, const ArchiveFile_t& a, const std::wstring& parserType) {
    int64_t archiveId = store.GetArchiveIdByPath(a.filePath);
    std::wstring entryErr;
    if (!store.BeginReplaceArchiveEntriesByArchiveId(archiveId, &entryErr)) {
        LOG_WARN(L"BeginReplaceArchiveEntriesByArchiveId failed: %s", entryErr.c_str());
        return;
    }

    std::unique_ptr<EveryZip::IArchiveParser> parserPtr =
        EveryZip::CreateArchiveParserByType(parserType);
    if (!parserPtr) {
        LOG_WARN(L"No archive parser for: %s (parser=%s)", a.filePath.c_str(), parserType.c_str());
        store.AbortReplaceArchiveEntriesByArchiveId(archiveId, &entryErr);
        return;
    }
    std::string perr;
    if (!parserPtr->Open(a.filePath, &perr)) {
        LOG_WARN(L"ArchiveParser::Open failed (%s): %s",
                 a.filePath.c_str(), Utf8ToWString(perr.c_str()).c_str());
        store.AbortReplaceArchiveEntriesByArchiveId(archiveId, &entryErr);
        return;
    }

    std::vector<ArchiveEntry_t> batch;
    batch.reserve(kParsedEntryBatchSize);
    auto flush = [&]() -> bool {
        if (batch.empty()) return true;
        if (!store.AppendArchiveEntriesByArchiveId(archiveId, batch, &entryErr)) {
            LOG_WARN(L"AppendArchiveEntriesByArchiveId failed: %s", entryErr.c_str());
            return false;
        }
        batch.clear();
        return true;
    };

    bool ok = parserPtr->ForEachEntry([&](const ArchiveEntry_t& e) {
        if (e.isDirectory) return true;
        if (e.entryPathUtf8.empty()) {
            LOG_WARN(L"Skip archive entry with empty UTF-8 path: %s", a.filePath.c_str());
            return true;
        }
        ArchiveEntry_t out;
        out.archiveId = archiveId;
        out.entryPathUtf8 = e.entryPathUtf8;
        out.entryRawPath = e.entryRawPath;
        out.compressedSize = e.compressedSize;
        out.originalSize = e.originalSize;
        out.modifiedTime = e.modifiedTime;
        batch.push_back(std::move(out));
        return batch.size() < kParsedEntryBatchSize || flush();
    }, &perr);
    parserPtr->Close();

    if (!ok) {
        LOG_WARN(L"ArchiveParser::ForEachEntry failed (%s): %s",
                 a.filePath.c_str(), Utf8ToWString(perr.c_str()).c_str());
        store.AbortReplaceArchiveEntriesByArchiveId(archiveId, &entryErr);
        return;
    }
    if (!flush()) return;
    if (!store.FinishReplaceArchiveEntriesByArchiveId(archiveId, &entryErr)) {
        LOG_WARN(L"FinishReplaceArchiveEntriesByArchiveId failed: %s", entryErr.c_str());
    }
}

static bool IsDriveAllowed(wchar_t driveLetter, const std::vector<wchar_t>& allowedDrives) {
    if (allowedDrives.empty()) return true;
    wchar_t normalized = driveLetter;
    if (normalized >= L'a' && normalized <= L'z') {
        normalized = (wchar_t)(normalized - L'a' + L'A');
    }
    for (wchar_t allowed : allowedDrives) {
        if (allowed >= L'a' && allowed <= L'z') {
            allowed = (wchar_t)(allowed - L'a' + L'A');
        }
        if (normalized == allowed) return true;
    }
    return false;
}

std::vector<wchar_t> Indexer::GetMonitoredDrives() const {
    std::vector<wchar_t> drives;
    DWORD needed = GetLogicalDriveStringsW(0, nullptr);
    if (needed == 0) return drives;

    std::wstring buf;
    buf.resize(needed);
    if (GetLogicalDriveStringsW(needed, buf.data()) == 0) return drives;

    const wchar_t* p = buf.c_str();
    while (*p) {
        std::wstring root = p;
        p += root.size() + 1;
        if (root.size() < 2) continue;

        UINT dtype = GetDriveTypeW(root.c_str());
        if (dtype == DRIVE_NO_ROOT_DIR || dtype == DRIVE_UNKNOWN) continue;

        // 检查是否为 NTFS
        wchar_t fsName[MAX_PATH] = {};
        if (!GetVolumeInformationW(root.c_str(), nullptr, 0, nullptr, nullptr, nullptr, fsName, MAX_PATH)) continue;
        if (_wcsicmp(fsName, L"NTFS") != 0) continue;

        wchar_t driveLetter = root[0];
        if (!IsDriveAllowed(driveLetter, scanDriveLetters_)) continue;

        drives.push_back(driveLetter);
    }
    return drives;
}

static void PostParseProgress(HWND hWnd, size_t done, size_t total) {
    PostMessageW(hWnd, WM_APP_PARSE_PROGRESS, (WPARAM)done, (LPARAM)total);
}

void Indexer::Stop() {
    LOG_INFO(L"StopIndexing requested");
    stage_.store((int)Stage::Stopping);
    cancel_.store(true);
    if (thread_.joinable()) {
        thread_.join();
    }
    running_.store(false);
    stage_.store((int)Stage::IdleMonitoring);
    LOG_INFO(L"StopIndexing done");
}

void Indexer::Start(HWND hWnd) {
    LOG_INFO(L"StartIndexing requested");
    Stop();
    cancel_.store(false);
    running_.store(true);
    stage_.store((int)Stage::InitialScanning);

    thread_ = std::thread([this, hWnd]() {
        FileScanner scanner;
        scanner.SetArchiveExtensions(archiveExtensions_);
        scanner.SetScanDriveLetters(scanDriveLetters_);
        std::vector<ArchiveFile_t> scanned;
        std::wstring err;
        const auto scanStart = std::chrono::steady_clock::now();
        const bool scanOk = scanner.Scan(&scanned, &err, &cancel_);
        const auto scanElapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - scanStart).count();
        if (!scanOk) {
            LOG_WARN(L"FileScanner::Scan failed: %s", err.c_str());
        }
        LOG_INFO(L"Initial archive scan completed in %.3f s", scanElapsed);

        if (!cancel_.load() && scanOk) {
            stage_.store((int)Stage::SyncingDatabase);
            const auto syncStart = std::chrono::steady_clock::now();
            auto store = CreateIndexStore();
            if (!store->OpenOrCreate(dbPath_, &err)) {
                LOG_WARN(L"IndexStore::Open failed: %s", err.c_str());
            } else if (!store->EnsureSchema(&err, true)) {
                LOG_WARN(L"IndexStore::EnsureSchema failed: %s", err.c_str());
            } else {
                LOG_INFO(L"SyncDB: OpenOrCreate+EnsureSchema %.3f s",
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - syncStart).count());
                std::vector<ArchiveFile_t> old;
                const auto queryStart = std::chrono::steady_clock::now();
                if (!store->QueryArchives(L"", &old, &err)) {
                    LOG_WARN(L"QueryArchives failed: %s", err.c_str());
                } else {
                    LOG_INFO(L"SyncDB: QueryArchives(%zu old) %.3f s", old.size(),
                        std::chrono::duration<double>(std::chrono::steady_clock::now() - queryStart).count());

                    static const char kSevenZipSizePolicyKey[] = "sevenzip_size_policy_version";
                    static const char kSevenZipSizePolicyVersion[] = "2";
                    std::string sevenZipPolicyVersion;
                    const bool reparseSevenZip =
                        !store->GetConfigValue(kSevenZipSizePolicyKey, &sevenZipPolicyVersion) ||
                        sevenZipPolicyVersion != kSevenZipSizePolicyVersion;

                    struct OldInfo {
                        ArchiveFile_t file;
                        bool seen = false;
                    };
                    std::unordered_map<std::wstring, OldInfo> oldMap;
                    oldMap.reserve(old.size());

                    auto makeKey = [](const ArchiveFile_t& af) -> std::wstring {
                        return af.driveLetter + L":" + std::to_wstring((unsigned long long)af.fileRefNumber);
                    };

                    for (const auto& o : old) {
                        oldMap.emplace(makeKey(o), OldInfo{ o, false });
                    }

                    std::vector<ArchiveFile_t> upserts;
                    std::vector<ArchiveFile_t> toParse;

                    for (const auto& cur : scanned) {
                        if (cancel_.load()) break;

                        const std::wstring key = makeKey(cur);
                        auto it = oldMap.find(key);
                        if (it == oldMap.end()) {
                            upserts.push_back(cur);
                            toParse.push_back(cur);
                            continue;
                        }

                        it->second.seen = true;
                        const auto& prev = it->second.file;
                        const bool changed = (cur.usn != prev.usn) ||
                                             (cur.modifiedTime != prev.modifiedTime) ||
                                             (cur.fileSize != prev.fileSize) ||
                                             (cur.filePath != prev.filePath) ||
                                             (reparseSevenZip && IsSevenZipPath(cur.filePath));
                        if (changed) {
                            if (!prev.filePath.empty()) {
                                std::wstring delErr;
                                if (!store->DeleteEntriesByArchivePath(prev.filePath, &delErr)) {
                                    LOG_WARN(L"DeleteEntriesByArchivePath failed: %s", delErr.c_str());
                                }
                            }
                            upserts.push_back(cur);
                            toParse.push_back(cur);
                        }
                    }

                    LOG_INFO(L"SyncDB: Diff scanned vs old -> upserts=%zu toParse=%zu %.3f s",
                        upserts.size(), toParse.size(),
                        std::chrono::duration<double>(std::chrono::steady_clock::now() - syncStart).count());

                    for (const auto& kv : oldMap) {
                        if (cancel_.load()) break;
                        const auto& prev = kv.second.file;
                        if (kv.second.seen) continue;

                        if (!prev.driveLetter.empty()) {
                            store->DeleteArchiveByRef(prev.driveLetter[0], (uint64_t)prev.fileRefNumber);
                        }
                        if (!prev.filePath.empty()) {
                            std::wstring delErr;
                            if (!store->DeleteEntriesByArchivePath(prev.filePath, &delErr)) {
                                LOG_WARN(L"DeleteEntriesByArchivePath failed: %s", delErr.c_str());
                            }
                        }
                    }

                    LOG_INFO(L"SyncDB: Delete gone archives %.3f s",
                        std::chrono::duration<double>(std::chrono::steady_clock::now() - syncStart).count());

                    std::vector<int64_t> archiveIds;

                    if (!cancel_.load() && !upserts.empty()) {
                        const auto upsertStart = std::chrono::steady_clock::now();
                        if (!store->UpsertArchives(upserts, &archiveIds, &err)) {
                            LOG_WARN(L"UpsertArchives failed: %s", err.c_str());
                        }
                        LOG_INFO(L"SyncDB: UpsertArchives(%zu) %.3f s", upserts.size(),
                            std::chrono::duration<double>(std::chrono::steady_clock::now() - upsertStart).count());
                    }

                    const size_t parseTotal = toParse.size();
                    size_t parseDone = 0;
                    size_t parsedEntryCount = 0;
                    const auto parseStart = std::chrono::steady_clock::now();
                    if (!cancel_.load() && parseTotal > 0) {
                        stage_.store((int)Stage::ParsingArchives);
                        PostParseProgress(hWnd, 0, parseTotal);
                    }
                    if (!cancel_.load() && parseTotal > 0) {
                        // archiveIds already obtained from UpsertArchives
                        // (upserts.size() == toParse.size(), same elements in same order)

                        const uint32_t resolvedParseThreads = ResolveParseThreadCount(parseThreadCount_);
                        const size_t workerCount = std::min<size_t>(parseTotal, resolvedParseThreads);
                        LOG_INFO(L"Initial archive parsing started: parse_threads=%u parse_total=%zu",
                                 (unsigned)workerCount, parseTotal);

                        struct ParseWorkState {
                            std::mutex mutex;
                            std::condition_variable cv;
                            size_t nextIndex = 0;
                            size_t activeWorkers = 0;
                            std::queue<ParsedEntryBatch> ready;
                        } work;
                        work.activeWorkers = workerCount;

                        const auto rules = archiveFormatRules_;
                        std::vector<std::thread> workers;
                        workers.reserve(workerCount);
                        for (size_t workerIndex = 0; workerIndex < workerCount; ++workerIndex) {
                            workers.emplace_back([&]() {
                                for (;;) {
                                    size_t taskIndex = 0;
                                    {
                                        std::lock_guard<std::mutex> lock(work.mutex);
                                        if (cancel_.load() || work.nextIndex >= parseTotal) {
                                            break;
                                        }
                                        taskIndex = work.nextIndex++;
                                    }

                                    const ArchiveFile_t archive = toParse[taskIndex];
                                    const int64_t archiveId = archiveIds[taskIndex];
                                    const std::wstring parserType = GetParserTypeForPath(archive.filePath, rules);

                                    auto pushBatch = [&](ParsedEntryBatch batch) {
                                        std::lock_guard<std::mutex> lock(work.mutex);
                                        work.ready.push(std::move(batch));
                                        work.cv.notify_one();
                                    };

                                    ParsedEntryBatch begin;
                                    begin.archive = archive;
                                    begin.archiveId = archiveId;
                                    begin.ok = archiveId >= 0;
                                    begin.done = false;
                                    if (!begin.ok) {
                                        LOG_WARN(L"GetArchiveIdByPath failed for: %s", archive.filePath.c_str());
                                        begin.done = true;
                                        pushBatch(std::move(begin));
                                        continue;
                                    }
                                    pushBatch(std::move(begin));

                                    std::unique_ptr<EveryZip::IArchiveParser> parserPtr =
                                        EveryZip::CreateArchiveParserByType(parserType);
                                    if (!parserPtr) {
                                        LOG_WARN(L"No archive parser for: %s (parser=%s)", archive.filePath.c_str(), parserType.c_str());
                                        ParsedEntryBatch done;
                                        done.archive = archive;
                                        done.archiveId = archiveId;
                                        done.ok = false;
                                        done.done = true;
                                        pushBatch(std::move(done));
                                        continue;
                                    }

                                    std::string perr;
                                    if (!parserPtr->Open(archive.filePath, &perr)) {
                                        LOG_WARN(L"ArchiveParser::Open failed (%s): %s",
                                                 archive.filePath.c_str(), Utf8ToWString(perr.c_str()).c_str());
                                        ParsedEntryBatch done;
                                        done.archive = archive;
                                        done.archiveId = archiveId;
                                        done.ok = false;
                                        done.done = true;
                                        pushBatch(std::move(done));
                                        continue;
                                    }

                                    ParsedEntryBatch current;
                                    current.archive = archive;
                                    current.archiveId = archiveId;
                                    current.ok = true;
                                    current.entries.reserve(kParsedEntryBatchSize);
                                    auto flushBatch = [&]() -> bool {
                                        if (current.entries.empty()) return true;
                                        ParsedEntryBatch out = std::move(current);
                                        pushBatch(std::move(out));
                                        current = ParsedEntryBatch{};
                                        current.archive = archive;
                                        current.archiveId = archiveId;
                                        current.ok = true;
                                        current.entries.reserve(kParsedEntryBatchSize);
                                        return true;
                                    };

                                    bool parseOk = parserPtr->ForEachEntry([&](const ArchiveEntry_t& e) {
                                        if (cancel_.load()) return false;
                                        if (e.isDirectory) return true;
                                        if (e.entryPathUtf8.empty()) {
                                            LOG_WARN(L"Skip archive entry with empty UTF-8 path: %s", archive.filePath.c_str());
                                            return true;
                                        }
                                        ArchiveEntry_t out;
                                        out.archiveId = archiveId;
                                        out.entryPathUtf8 = e.entryPathUtf8;
                                        out.entryRawPath = e.entryRawPath;
                                        out.compressedSize = e.compressedSize;
                                        out.originalSize = e.originalSize;
                                        out.modifiedTime = e.modifiedTime;
                                        current.entries.push_back(std::move(out));
                                        return current.entries.size() < kParsedEntryBatchSize || flushBatch();
                                    }, &perr);
                                    parserPtr->Close();
                                    if (parseOk) {
                                        flushBatch();
                                    } else if (!cancel_.load()) {
                                        LOG_WARN(L"ArchiveParser::ForEachEntry failed (%s): %s",
                                                 archive.filePath.c_str(), Utf8ToWString(perr.c_str()).c_str());
                                    }

                                    ParsedEntryBatch done;
                                    done.archive = archive;
                                    done.archiveId = archiveId;
                                    done.ok = parseOk;
                                    done.done = true;
                                    pushBatch(std::move(done));
                                }

                                {
                                    std::lock_guard<std::mutex> lock(work.mutex);
                                    if (work.activeWorkers > 0) {
                                        --work.activeWorkers;
                                    }
                                }
                                work.cv.notify_one();
                            });
                        }

                        for (;;) {
                            ParsedEntryBatch result;
                            bool hasResult = false;
                            {
                                std::unique_lock<std::mutex> lock(work.mutex);
                                work.cv.wait(lock, [&]() {
                                    return !work.ready.empty() || work.activeWorkers == 0;
                                });
                                if (!work.ready.empty()) {
                                    result = std::move(work.ready.front());
                                    work.ready.pop();
                                    hasResult = true;
                                } else if (work.activeWorkers == 0) {
                                    break;
                                }
                            }

                            if (!hasResult) continue;
                            if (!cancel_.load() && result.archiveId >= 0) {
                                std::wstring entryErr;
                                if (!result.done && result.entries.empty()) {
                                    if (!store->BeginReplaceArchiveEntriesByArchiveId(result.archiveId, &entryErr)) {
                                        LOG_WARN(L"BeginReplaceArchiveEntriesByArchiveId failed: %s", entryErr.c_str());
                                    }
                                } else if (!result.entries.empty()) {
                                    parsedEntryCount += result.entries.size();
                                    if (!store->AppendArchiveEntriesByArchiveId(result.archiveId, result.entries, &entryErr)) {
                                        LOG_WARN(L"AppendArchiveEntriesByArchiveId failed: %s", entryErr.c_str());
                                    }
                                } else if (result.done) {
                                    if (result.ok) {
                                        if (!store->FinishReplaceArchiveEntriesByArchiveId(result.archiveId, &entryErr)) {
                                            LOG_WARN(L"FinishReplaceArchiveEntriesByArchiveId failed: %s", entryErr.c_str());
                                        }
                                    } else if (!store->AbortReplaceArchiveEntriesByArchiveId(result.archiveId, &entryErr)) {
                                        LOG_WARN(L"AbortReplaceArchiveEntriesByArchiveId failed: %s", entryErr.c_str());
                                    }
                                }
                            }
                            if (result.done) {
                                ++parseDone;
                                PostParseProgress(hWnd, parseDone, parseTotal);
                            }
                        }

                        for (auto& worker : workers) {
                            if (worker.joinable()) {
                                worker.join();
                            }
                        }
                    }
                    const auto parseElapsed = std::chrono::duration<double>(
                        std::chrono::steady_clock::now() - parseStart).count();
                    const double archivesPerSec = parseElapsed > 0.0
                        ? static_cast<double>(parseDone) / parseElapsed
                        : 0.0;
                    const double entriesPerSec = parseElapsed > 0.0
                        ? static_cast<double>(parsedEntryCount) / parseElapsed
                        : 0.0;
                    LOG_INFO(L"Initial archive parsing completed: parse_threads=%u parse_total=%zu parse_done=%zu entries=%zu parse_elapsed_sec=%.3f archives_per_sec=%.2f entries_per_sec=%.2f",
                             (unsigned)(parseTotal > 0 ? std::min<size_t>(parseTotal, ResolveParseThreadCount(parseThreadCount_)) : 0),
                             parseTotal, parseDone, parsedEntryCount, parseElapsed, archivesPerSec, entriesPerSec);
                    if (!cancel_.load()) {
                        stage_.store((int)Stage::SyncingDatabase);
                    }

                    if (reparseSevenZip && !cancel_.load()) {
                        std::wstring cfgErr;
                        if (!store->SaveConfigValue(kSevenZipSizePolicyKey, kSevenZipSizePolicyVersion, &cfgErr)) {
                            LOG_WARN(L"Save sevenzip size policy version failed: %s", cfgErr.c_str());
                        }
                    }

                    if (!cancel_.load() && parsedEntryCount > 0) {
                        const auto compactStart = std::chrono::steady_clock::now();
                        LOG_INFO(L"Initial archive parsing compact started: entries=%zu", parsedEntryCount);
                        if (!store->Compact()) {
                            LOG_WARN(L"Initial archive parsing compact failed");
                        } else {
                            const auto compactElapsed = std::chrono::duration<double>(
                                std::chrono::steady_clock::now() - compactStart).count();
                            LOG_INFO(L"Initial archive parsing compact completed: elapsed_sec=%.3f", compactElapsed);
                        }
                    }
                }

                // 初始扫描完成后，记录每个盘符的 Journal NextUsn 作为监控起点
                if (!cancel_.load()) {
                    auto drives = GetMonitoredDrives();
                    for (wchar_t dl : drives) {
                        JournalInfo ji;
                        if (FileScanner::QueryJournalInfo(dl, &ji, &err)) {
                            store->SaveJournalUsn(dl, ji.journalId, ji.nextUsn, &err);
                            LOG_INFO(L"Saved Journal USN for drive %c: journalId=%lld, nextUsn=%lld",
                                     dl, (long long)ji.journalId, (long long)ji.nextUsn);
                        }
                    }
                }

            }
        }

        // 标记初始索引完成，通知 UI 刷新
        running_.store(false);
        stage_.store((int)Stage::IdleMonitoring);
        PostMessageW(hWnd, WM_APP_DB_REFRESH, 0, 0);
        PostMessageW(hWnd, WM_APP_INDEX_DONE, 0, 0);

        // ═══════════════════════════════════════════════════════════
        //  监控循环：定期读取 USN Journal 增量变化，实时同步数据库
        // ═══════════════════════════════════════════════════════════
        LOG_INFO(L"Entering USN Journal monitoring loop");

        auto monitorStore = CreateIndexStore();
        {
            std::wstring monErr;
            if (!monitorStore->OpenOrCreate(dbPath_, &monErr)) {
                LOG_WARN(L"Monitor: IndexStore::Open failed: %s", monErr.c_str());
            }
        }

        while (!cancel_.load() && monitorStore->IsOpen()) {
            // 每 2 秒检查一次变化
            for (int i = 0; i < 20 && !cancel_.load(); ++i) {
                Sleep(100);
            }
            if (cancel_.load()) break;

            std::wstring err;
            auto drives = GetMonitoredDrives();
            bool anyChanged = false;

            for (wchar_t dl : drives) {
                if (cancel_.load()) break;

                // 读取上次保存的 Journal 位置
                int64_t savedJournalId = 0;
                USN savedNextUsn = 0;
                monitorStore->GetJournalUsn(dl, &savedJournalId, &savedNextUsn);

                if (savedJournalId == 0 && savedNextUsn == 0) {
                    // 没有保存过，跳过（等待下次全量扫描）
                    continue;
                }

                // 增量读取 USN Journal 变化
                std::vector<UsnChangeRecord_t> changes;
                USN newNextUsn = 0;
                std::wstring scanErr;
                if (!FileScanner::ScanUsnJournal(dl, savedJournalId, savedNextUsn,
                                                  &changes, &newNextUsn, &scanErr, &cancel_,
                                                  &archiveExtensions_)) {
                    LOG_WARN(L"Monitor: ScanUsnJournal failed for %c: %s", dl, scanErr.c_str());
                    continue;
                }

                if (changes.empty()) {
                    // 即使没有归档文件变化，也更新 USN 位置避免重复扫描
                    if (newNextUsn > savedNextUsn) {
                        monitorStore->SaveJournalUsn(dl, savedJournalId, newNextUsn, &err);
                    }
                    continue;
                }

                LOG_INFO(L"Monitor: %zu USN changes detected on drive %c", changes.size(), dl);

                // 按 fileRefNumber 去重，只保留每个文件的最后一条记录
                std::unordered_map<uint64_t, UsnChangeRecord_t> deduped;
                for (auto& cr : changes) {
                    deduped[(uint64_t)cr.fileRefNumber] = std::move(cr);
                }

                // 打开卷句柄，在同一盘符的所有变化记录中复用
                wchar_t volPath[] = L"\\\\.\\X:";
                volPath[4] = dl;
                HANDLE hVol = CreateFileW(volPath, GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, 0, nullptr);

                for (const auto& kv : deduped) {
                    if (cancel_.load()) break;
                    const auto& cr = kv.second;

                    bool isDelete = (cr.reason & USN_REASON_FILE_DELETE) != 0;
                    bool isRenameOld = (cr.reason & USN_REASON_RENAME_OLD_NAME) != 0;

                    if (isDelete || isRenameOld) {
                        // 文件被删除或重命名（旧名）：从数据库中移除
                        ArchiveFile_t oldAf;
                        if (monitorStore->GetArchiveByRef(dl, (uint64_t)cr.fileRefNumber, &oldAf)) {
                            LOG_INFO(L"Monitor: Archive deleted/renamed: %s", oldAf.filePath.c_str());
                            if (!oldAf.filePath.empty()) {
                                std::wstring delErr;
                                monitorStore->DeleteEntriesByArchivePath(oldAf.filePath, &delErr);
                            }
                            monitorStore->DeleteArchiveByRef(dl, (uint64_t)cr.fileRefNumber);
                            anyChanged = true;
                        }
                    } else {
                        // 文件新增或修改：复用 FileScanner::GetFileInfoByRefNumber 获取元数据
                        if (hVol == INVALID_HANDLE_VALUE) continue;

                        uint64_t fileSize = 0;
                        uint64_t modifyTime = 0;
                        std::wstring fullPath;
                        if (!FileScanner::GetFileInfoByRefNumber(hVol, (uint64_t)cr.fileRefNumber,
                                                                  &fileSize, &modifyTime, &fullPath)) {
                            // 文件可能已被删除，清理数据库
                            ArchiveFile_t oldAf;
                            if (monitorStore->GetArchiveByRef(dl, (uint64_t)cr.fileRefNumber, &oldAf)) {
                                if (!oldAf.filePath.empty()) {
                                    std::wstring delErr;
                                    monitorStore->DeleteEntriesByArchivePath(oldAf.filePath, &delErr);
                                }
                                monitorStore->DeleteArchiveByRef(dl, (uint64_t)cr.fileRefNumber);
                                anyChanged = true;
                            }
                            continue;
                        }

                        if (fullPath.empty()) continue;

                        // 先删除旧的条目（如果路径变了）
                        ArchiveFile_t oldAf;
                        if (monitorStore->GetArchiveByRef(dl, (uint64_t)cr.fileRefNumber, &oldAf)) {
                            if (!oldAf.filePath.empty() && oldAf.filePath != fullPath) {
                                std::wstring delErr;
                                monitorStore->DeleteEntriesByArchivePath(oldAf.filePath, &delErr);
                            }
                        }

                        // 更新 archives 表
                        ArchiveFile_t af;
                        af.driveLetter = std::wstring(1, dl);
                        af.filePath = fullPath;
                        af.fileSize = fileSize;
                        af.modifiedTime = modifyTime;
                        af.fileRefNumber = cr.fileRefNumber;
                        af.usn = cr.usn;
                        monitorStore->UpsertArchive(af);

                        // 重新解析归档文件内容
                        LOG_INFO(L"Monitor: Re-parsing archive: %s", fullPath.c_str());
                        stage_.store((int)Stage::ParsingArchives);
                        PostParseProgress(hWnd, 0, 1);
                        ParseAndStoreArchive(*monitorStore, af, GetParserTypeForPath(af.filePath, archiveFormatRules_));
                        PostParseProgress(hWnd, 1, 1);
                        stage_.store((int)Stage::IdleMonitoring);
                        anyChanged = true;
                    }
                }

                if (hVol != INVALID_HANDLE_VALUE) {
                    CloseHandle(hVol);
                }

                // 更新 Journal USN 位置
                if (newNextUsn > savedNextUsn) {
                    monitorStore->SaveJournalUsn(dl, savedJournalId, newNextUsn, &err);
                }
            }

            // 如果有变化，通知 UI 刷新
            if (anyChanged && !cancel_.load()) {
                const auto compactStart = std::chrono::steady_clock::now();
                LOG_INFO(L"Monitor compact started after changes");
                if (!monitorStore->Compact()) {
                    LOG_WARN(L"Monitor compact failed");
                } else {
                    const auto compactElapsed = std::chrono::duration<double>(
                        std::chrono::steady_clock::now() - compactStart).count();
                    LOG_INFO(L"Monitor compact completed: elapsed_sec=%.3f", compactElapsed);
                }
                PostMessageW(hWnd, WM_APP_DB_REFRESH, 0, 0);
            }
        }

        LOG_INFO(L"USN Journal monitoring loop exited");
        stage_.store((int)Stage::IdleMonitoring);
    });
}
