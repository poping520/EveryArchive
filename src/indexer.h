#pragma once

#include <windows.h>
#include <winioctl.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "file_scanner.h"
#include "index_store.h"
#include "config/user_config.h"

class Indexer {
public:
    enum class Stage {
        IdleMonitoring = 0,
        InitialScanning,
        SyncingDatabase,
        ParsingArchives,
        Stopping,
    };

    /** 构造索引器对象。 */
    Indexer();
    /** 析构索引器并停止后台线程。 */
    ~Indexer();

    Indexer(const Indexer&) = delete;
    Indexer& operator=(const Indexer&) = delete;

    /**
     * 设置数据库路径。
     * @param dbPath 索引数据库文件路径。
     */
    void SetDbPath(const std::wstring& dbPath);

    /**
     * 设置归档文件扩展名（从 UserConfig 加载后传入）。
     * @param exts 需要识别为归档文件的扩展名列表。
     */
    void SetArchiveExtensions(const std::vector<std::wstring>& exts);

    void SetArchiveFormatRules(const std::vector<UserConfig::ArchiveFormatRule>& rules);

    /**
     * 设置限定扫描和监控的盘符列表。为空时扫描所有 NTFS 盘。
     * @param drives 需要扫描的盘符列表。
     */
    void SetScanDriveLetters(const std::vector<wchar_t>& drives);

    void SetParseThreadCount(uint32_t threads);

    void SetAppStartTickMs(uint64_t tickMs);

    /**
     * 确保数据库文件存在并创建所需的表。
     * @return 准备成功返回 true，否则返回 false。
     */
    bool EnsureDatabaseReady();

    /**
     * 启动后台索引线程：扫描磁盘、增量更新数据库、解析归档条目并进入监控循环。
     * @param hWnd 用于向 UI 线程发送刷新消息的窗口句柄。
     */
    void Start(HWND hWnd);

    /** 停止后台索引线程（设置取消标志并等待线程结束）。 */
    void Stop();

    /**
     * 获取当前运行状态。
     * @return 后台索引线程运行中返回 true，否则返回 false。
     */
    bool IsRunning() const { return running_.load(); }
    Stage GetStage() const { return static_cast<Stage>(stage_.load()); }

    /**
     * 提升进程权限（用于 USN Journal 扫描需要的 SeManageVolume / SeBackup）。
     * @param privilegeName 需要启用的 Windows 权限名称。
     * @return 启用成功返回 true，否则返回 false。
     */
    static bool EnablePrivilege(const wchar_t* privilegeName);

private:
    /**
     * 解析单个归档文件并将条目写入数据库（先删除旧条目再插入新条目）。
     * @param db 已打开的数据库连接。
     * @param a 待解析的归档文件记录。
     */
    static void ParseAndStoreArchive(IndexStore& store, const ArchiveFile_t& a, const std::wstring& parserType);

    /**
     * 获取所有需要监控的 NTFS 盘符列表。
     * @return 可用于 USN 监控的盘符列表。
     */
    std::vector<wchar_t> GetMonitoredDrives() const;

    std::wstring dbPath_;
    std::vector<std::wstring> archiveExtensions_ = { L".zip", L".7z", L".rar" };
    std::vector<UserConfig::ArchiveFormatRule> archiveFormatRules_;
    std::vector<wchar_t> scanDriveLetters_;
    uint32_t parseThreadCount_ = 0;
    std::atomic<uint64_t> appStartTickMs_{ 0 };
    std::atomic_bool cancel_{ false };
    std::atomic_bool running_{ false };
    std::atomic<int> stage_{ (int)Stage::IdleMonitoring };
    std::thread thread_;
};
