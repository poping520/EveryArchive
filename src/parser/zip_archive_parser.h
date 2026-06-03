#pragma once

#include <cstdint>
#include <string>

#include "archive_parser.h"

namespace EveryZip {

class ZipArchiveParser final : public IArchiveParser {
public:
    /** 构造 ZIP 归档解析器对象。 */
    ZipArchiveParser();
    /** 析构解析器并自动关闭已打开的归档。 */
    ~ZipArchiveParser() override;

    /**
     * 打开指定 ZIP 归档文件。
     * @param archive_path ZIP 文件路径。
     * @param error 可选，用于输出错误信息。
     * @return 打开成功返回 true，否则返回 false。
     */
    bool Open(const std::wstring& archive_path, std::string* error) override;

    /** 关闭当前 ZIP 归档并释放句柄。 */
    void Close() override;

    /**
     * 判断当前是否已成功打开 ZIP 归档。
     * @return 已打开返回 true，否则返回 false。
     */
    bool IsOpen() const override;

    /**
     * 获取当前打开的 ZIP 文件路径。
     * @return 归档文件路径；未打开时返回空字符串。
     */
    std::wstring ArchivePath() const override;

    /**
     * 列出 ZIP 归档中的全部条目。
     * @param out_entries 输出条目列表。
     * @param error 可选，用于输出错误信息。
     * @return 枚举成功返回 true，否则返回 false。
     */
    bool ListEntries(std::vector<ArchiveEntry_t>* out_entries, std::string* error) override;
    bool ForEachEntry(const std::function<bool(const ArchiveEntry_t&)>& visitor,
                      std::string* error) override;

    /**
     * 将 ZIP 归档内指定条目解压到目标目录。
     * @param entry_path 归档内条目路径。
     * @param dest_dir   解压目标目录。
     * @param error      可选，用于输出错误信息。
     * @return 解压成功返回 true，否则返回 false。
     */
    bool ExtractEntry(const std::string& entry_path,
                      const std::wstring& dest_dir,
                      std::string* error) override;

    static bool EstimateEntryCount(const std::wstring& archive_path,
                                   std::uint64_t* out_entry_count,
                                   std::string* error);

private:
    void* handle_ = nullptr; // unzFile
    std::wstring archive_path_;
};

} // namespace EveryZip
