#include "zip_archive_parser.h"
#include "zip_cd_scanner.h"

#include "../string_utils.h"

#include <windows.h>

#include <algorithm>
#include <cstring>

extern "C" {
#include "ioapi.h"
#include "iowin32.h"
#include "unzip.h"
}

namespace EveryZip {

 // 判断归档条目名是否以目录分隔符结尾，以识别目录项。
 // 参数：s - 归档内原始条目名称。
 // 返回值：目录项返回 true，否则返回 false。
static bool EndsWithSlash(const std::string& s) {
    if (s.empty()) return false;
    const char c = s.back();
    return c == '/' || c == '\\';
}

struct DecodedZipPath {
    std::string utf8;
};

// 将 ZIP 条目名称解码为用于入库的 UTF-8 字符串。
static DecodedZipPath DecodeZipPathBestEffort(const std::string& s, bool isUtf8) {
    DecodedZipPath result;
    if (s.empty()) return result;

    if (isUtf8) {
        if (!MultiByteToWString(s, CP_UTF8, MB_ERR_INVALID_CHARS).empty()) {
            result.utf8 = s;
            return result;
        }
        result.utf8 = WideToUtf8(MultiByteToWString(s, CP_ACP));
        return result;
    }

    std::wstring wide = MultiByteToWString(s, CP_ACP);
    if (wide.empty()) {
        wide = MultiByteToWString(s, CP_UTF8);
    }
    result.utf8 = WideToUtf8(wide);
    return result;
}

static std::wstring ToWideBestEffort(const std::string& s, bool isUtf8) {
    if (isUtf8) {
        std::wstring wide = MultiByteToWString(s, CP_UTF8, MB_ERR_INVALID_CHARS);
        if (!wide.empty()) return wide;
        return MultiByteToWString(s, CP_ACP);
    }
    std::wstring wide = MultiByteToWString(s, CP_ACP);
    if (!wide.empty()) return wide;
    return MultiByteToWString(s, CP_UTF8);
}

static bool GetCurrentFileInfoWithName(unzFile handle,
                                       unz_file_info64* info,
                                       std::string* name,
                                       std::string* error) {
    if (!info || !name) {
        if (error) *error = "invalid output argument";
        return false;
    }

    int rc = unzGetCurrentFileInfo64(handle, info, nullptr, 0, nullptr, 0, nullptr, 0);
    if (rc != UNZ_OK) {
        if (error) *error = "unzGetCurrentFileInfo64 failed";
        return false;
    }

    std::vector<char> namebuf((size_t)info->size_filename + 1, '\0');
    rc = unzGetCurrentFileInfo64(handle, info, namebuf.data(), (uLong)namebuf.size(), nullptr, 0, nullptr, 0);
    if (rc != UNZ_OK) {
        if (error) *error = "unzGetCurrentFileInfo64 failed";
        return false;
    }

    name->assign(namebuf.data(), info->size_filename);
    return true;
}

ZipArchiveParser::ZipArchiveParser() = default;

ZipArchiveParser::~ZipArchiveParser() {
    Close();
}

bool ZipArchiveParser::Open(const std::wstring& archive_path, std::string* error) {
    Close();

    if (archive_path.empty()) {
        if (error) *error = "archive path is empty";
        return false;
    }

    zlib_filefunc64_def filefunc{};
    fill_win32_filefunc64W(&filefunc);

    unzFile uf = unzOpen2_64(archive_path.c_str(), &filefunc);
    if (!uf) {
        if (error) *error = "unzOpen2_64 failed";
        return false;
    }

    handle_ = uf;
    archive_path_ = archive_path;
    return true;
}

void ZipArchiveParser::Close() {
    if (handle_) {
        unzClose((unzFile)handle_);
        handle_ = nullptr;
    }
    archive_path_.clear();
}

bool ZipArchiveParser::IsOpen() const {
    return handle_ != nullptr;
}

std::wstring ZipArchiveParser::ArchivePath() const {
    return archive_path_;
}

bool ZipArchiveParser::EstimateEntryCount(const std::wstring& archive_path,
                                          std::uint64_t* out_entry_count,
                                          std::string* error) {
    if (!out_entry_count) {
        if (error) *error = "out_entry_count is null";
        return false;
    }
    *out_entry_count = 0;
    if (archive_path.empty()) {
        if (error) *error = "archive path is empty";
        return false;
    }

    zlib_filefunc64_def filefunc{};
    fill_win32_filefunc64W(&filefunc);

    unzFile uf = unzOpen2_64(archive_path.c_str(), &filefunc);
    if (!uf) {
        if (error) *error = "unzOpen2_64 failed";
        return false;
    }

    unz_global_info64 info{};
    const int rc = unzGetGlobalInfo64(uf, &info);
    unzClose(uf);
    if (rc != UNZ_OK) {
        if (error) *error = "unzGetGlobalInfo64 failed";
        return false;
    }

    *out_entry_count = static_cast<std::uint64_t>(info.number_entry);
    return true;
}

bool ZipArchiveParser::ListEntries(std::vector<ArchiveEntry_t>* out_entries, std::string* error) {
    if (!out_entries) {
        if (error) *error = "out_entries is null";
        return false;
    }
    out_entries->clear();
    return ForEachEntry([&](const ArchiveEntry_t& entry) {
        out_entries->push_back(entry);
        return true;
    }, error);
}

bool ZipArchiveParser::ForEachEntry(const std::function<bool(const ArchiveEntry_t&)>& visitor,
                                    std::string* error) {
    if (!visitor) {
        if (error) *error = "visitor is null";
        return false;
    }
    if (!handle_) {
        if (error) *error = "archive is not open";
        return false;
    }

    // Fast path: try the Central Directory scanner first.
    if (!archive_path_.empty()) {
        struct CdScanCtx {
            const std::function<bool(const ArchiveEntry_t&)>* visitor;
            bool stopped;
        };
        CdScanCtx ctx;
        ctx.visitor = &visitor;
        ctx.stopped = false;

        auto cd_callback = [](const ZipCdEntry* cd, void* ud) -> int {
            auto* c = static_cast<CdScanCtx*>(ud);
            if (c->stopped) return 0;

            const std::string name(cd->raw_name, cd->raw_name_len);
            const bool isUtf8 = (cd->flags & 0x800) != 0;

            ArchiveEntry_t e;
            e.entryPathUtf8 = DecodeZipPathBestEffort(name, isUtf8).utf8;
            if (name != e.entryPathUtf8) {
                e.entryRawPath = name;
            }
            e.isDirectory = EndsWithSlash(name);
            e.compressedSize = cd->compressed_size;
            e.originalSize = cd->original_size;
            e.modifiedTime = cd->modified_time;

            if (!(*c->visitor)(e)) {
                c->stopped = true;
                return 0;
            }
            return 1;
        };

        char cd_error[128] = {};
        if (zip_cd_scan_entries(archive_path_.c_str(), cd_callback, &ctx, cd_error, sizeof(cd_error))) {
            if (ctx.stopped) {
                if (error) *error = "entry visitor stopped";
                return false;
            }
            return true;
        }
        // CD scanner failed — fall through to MiniZIP.
    }

    // Fallback: MiniZIP iteration.
    int rc = unzGoToFirstFile((unzFile)handle_);
    if (rc != UNZ_OK) {
        if (rc == UNZ_END_OF_LIST_OF_FILE) return true;
        if (error) *error = "unzGoToFirstFile failed";
        return false;
    }

    while (rc == UNZ_OK) {
        unz_file_info64 info{};
        std::string name;
        if (!GetCurrentFileInfoWithName((unzFile)handle_, &info, &name, error)) {
            return false;
        }

        const bool isUtf8 = (info.flag & 0x800) != 0;
        DecodedZipPath decodedPath = DecodeZipPathBestEffort(name, isUtf8);
        ArchiveEntry_t e;
        e.entryPathUtf8 = std::move(decodedPath.utf8);
        if (name != e.entryPathUtf8) {
            e.entryRawPath = name;
        }

        e.isDirectory = EndsWithSlash(name);
        e.compressedSize = (std::int64_t)info.compressed_size;
        e.originalSize = (std::uint64_t)info.uncompressed_size;

        std::tm t{};
        t.tm_sec = info.tmu_date.tm_sec;
        t.tm_min = info.tmu_date.tm_min;
        t.tm_hour = info.tmu_date.tm_hour;
        t.tm_mday = info.tmu_date.tm_mday;
        t.tm_mon = info.tmu_date.tm_mon;
        t.tm_year = info.tmu_date.tm_year - 1900;
        e.modifiedTime = LocalTmToFileTimeValue(t);

        if (!visitor(e)) {
            if (error) *error = "entry visitor stopped";
            return false;
        }

        rc = unzGoToNextFile((unzFile)handle_);
        if (rc == UNZ_END_OF_LIST_OF_FILE) break;
        if (rc != UNZ_OK) {
            if (error) *error = "unzGoToNextFile failed";
            return false;
        }
    }

    return true;
}

bool ZipArchiveParser::ExtractEntry(const std::string& entry_path,
                                    const std::wstring& dest_dir,
                                    std::string* error) {
    if (!handle_) {
        if (error) *error = "archive is not open";
        return false;
    }
    if (entry_path.empty()) {
        if (error) *error = "entry_path is empty";
        return false;
    }

    // 定位到目标条目（不区分大小写）
    int rc = unzLocateFile((unzFile)handle_, entry_path.c_str(), 2);
    if (rc != UNZ_OK) {
        if (error) *error = "entry not found: " + entry_path;
        return false;
    }

    // 获取条目信息（用于判断是否目录）
    unz_file_info64 info{};
    std::string name;
    if (!GetCurrentFileInfoWithName((unzFile)handle_, &info, &name, error)) {
        return false;
    }
    if (EndsWithSlash(name)) {
        // 纯目录条目，直接创建目录即可
        const bool isUtf8dir = (info.flag & 0x800) != 0;
        std::wstring dirPath = dest_dir + L"\\" + ToWideBestEffort(name, isUtf8dir);
        // 替换正斜杠为反斜杠
        std::replace(dirPath.begin(), dirPath.end(), L'/', L'\\');
        // 移除末尾斜杠
        while (!dirPath.empty() && (dirPath.back() == L'\\' || dirPath.back() == L'/'))
            dirPath.pop_back();
        CreateDirectoryW(dirPath.c_str(), nullptr);
        return true;
    }

    // 只取文件名部分（去掉归档内路径结构）
    std::string basename = name;
    size_t slashPos = basename.find_last_of("/\\");
    if (slashPos != std::string::npos)
        basename = basename.substr(slashPos + 1);
    if (basename.empty()) {
        if (error) *error = "entry has no filename: " + entry_path;
        return false;
    }

    std::wstring destPath = dest_dir;
    if (!destPath.empty() && destPath.back() != L'\\') destPath += L'\\';
    const bool isUtf8file = (info.flag & 0x800) != 0;
    destPath += ToWideBestEffort(basename, isUtf8file);

    // 打开当前条目准备读取
    rc = unzOpenCurrentFile((unzFile)handle_);
    if (rc != UNZ_OK) {
        if (error) *error = "unzOpenCurrentFile failed";
        return false;
    }

    // 创建目标文件
    HANDLE hFile = CreateFileW(destPath.c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        unzCloseCurrentFile((unzFile)handle_);
        if (error) *error = "CreateFileW failed for: " + entry_path;
        return false;
    }

    // 循环读取并写入
    bool ok = true;
    std::vector<char> buf(65536);
    for (;;) {
        int bytes = unzReadCurrentFile((unzFile)handle_, buf.data(), (unsigned)buf.size());
        if (bytes < 0) {
            if (error) *error = "unzReadCurrentFile failed";
            ok = false;
            break;
        }
        if (bytes == 0) break;
        DWORD written = 0;
        if (!WriteFile(hFile, buf.data(), (DWORD)bytes, &written, nullptr) || written != (DWORD)bytes) {
            if (error) *error = "WriteFile failed";
            ok = false;
            break;
        }
    }

    CloseHandle(hFile);
    int crcRc = unzCloseCurrentFile((unzFile)handle_);
    if (ok && crcRc == UNZ_CRCERROR) {
        if (error) *error = "CRC mismatch for: " + entry_path;
        return false;
    }
    return ok;
}

} // namespace EveryZip
