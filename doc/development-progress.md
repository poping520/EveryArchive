# EveryZip 当前开发进度

## 总体状态

项目已经形成可运行的 Windows 桌面 MVP 骨架：启动后创建主窗口和数据库，后台扫描 NTFS 盘中的目标归档文件，解析 ZIP/7z/RAR 内部条目并写入 SQLite，前台通过虚拟 ListView 搜索、排序和展示结果。后台还具备基于 USN Journal 的增量监控循环。

当前完成度可以概括为：

- 核心扫描链路：已实现。
- ZIP/7z/RAR 条目解析：已实现。
- SQLite 索引库：已实现。
- 查询展示 UI：已实现。
- 右键菜单基础操作：已实现多项。
- 配置文件基础设施：已实现。
- 多格式归档：7z 已通过 7-Zip C SDK 接入，RAR 已通过 RARLAB UnRAR 源码接入；libarchive 路线已放弃。
- 设置页、更新检查、双击行为：仍是占位或未完成。

## 模块进度

### 1. 程序入口与生命周期

对应文件：

- `src/main.cpp`
- `src/app.manifest`

已完成：

- Win32 `wWinMain` 入口。
- 单实例检测，已运行时激活现有窗口。
- 初始化日志、配置、数据库路径。
- 请求启用 `SeManageVolumePrivilege` 和 `SeBackupPrivilege`。
- 初始化 Common Controls。
- 创建主窗口并进入消息循环。
- Manifest 请求管理员权限。
- Manifest 启用 Common Controls v6。
- Manifest 声明 PerMonitor DPI 感知。

待完善：

- 自定义应用图标当前仍使用系统默认图标。
- 启动失败时的用户可读诊断还较基础。

### 2. 文件扫描模块

对应文件：

- `src/file_scanner.h`
- `src/file_scanner.cpp`
- `src/types.h`

已完成：

- 枚举本机逻辑盘。
- 过滤非 NTFS 盘。
- 使用 `FSCTL_QUERY_USN_JOURNAL` 查询 Journal 信息。
- 使用 `FSCTL_ENUM_USN_DATA` 扫描 NTFS 文件记录。
- 按扩展名匹配归档文件。
- 通过 FileReferenceNumber 获取文件大小、修改时间和 DOS 完整路径。
- 支持取消标志，中断长时间扫描。
- 支持配置传入的归档扩展名列表。
- 支持配置传入的扫描盘符列表；当前测试默认只扫描 G 盘，配置为空列表时扫描所有 NTFS 盘。
- 实现 `ScanUsnJournal`，用于从指定 USN 起点读取增量变化。

当前支持的默认扩展名：

- `.zip`
- `.7z`
- `.rar`

待完善：

- Journal ID 变化时需要自动触发全量重扫。
- 需要加入排除目录/排除盘符。
- 对无权限、离线盘、网络盘、异常卷的错误提示可继续细化。

### 3. 索引器模块

对应文件：

- `src/indexer.h`
- `src/indexer.cpp`

已完成：

- 后台线程执行索引任务。
- 启动前确保数据库和表结构存在。
- 全量扫描后与旧数据库记录对比。
- 新增归档：写入 `archives` 并解析条目。
- 修改归档：删除旧条目，更新归档元数据，重新解析条目。
- 删除归档：按盘符和 FileReferenceNumber 删除归档记录，并删除关联条目。
- 初始扫描完成后保存各盘符 Journal 起点。
- 进入 USN Journal 增量监控循环。
- 增量变化中处理新增、修改、删除、重命名等场景。
- 向 UI 发送刷新消息。
- 提供 `Start`、`Stop`、`IsRunning` 和权限提升工具函数。

待完善：

- 当前已按扩展名分发到 `ZipArchiveParser`、`SevenZipArchiveParser` 或 `RarArchiveParser`。
- 大量变化时仍是同步逐个解析，可进一步拆成任务队列。
- 增量监控失败后的恢复策略还需要产品化。

### 4. 归档解析模块

对应文件：

- `src/parser/archive_parser.h`
- `src/parser/zip_archive_parser.h`
- `src/parser/zip_archive_parser.cpp`
- `src/parser/sevenzip_archive_parser.h`
- `src/parser/sevenzip_archive_parser.cpp`
- `src/parser/rar_archive_parser.h`
- `src/parser/rar_archive_parser.cpp`
- `src/parser/archive_parser_factory.h`
- `src/parser/archive_parser_factory.cpp`
- `src/parser/libarchive_parser.h`（历史草稿，不再作为接入路线）
- `src/parser/libarchive_parser.cpp`（历史草稿，不再作为接入路线）

已完成：

- 定义统一解析器接口 `IArchiveParser`。
- `ZipArchiveParser` 基于 minizip 打开 ZIP 类归档。
- `SevenZipArchiveParser` 基于 7-Zip C SDK 打开 7z 归档。
- `RarArchiveParser` 基于 RARLAB UnRAR 源码打开 RAR 归档，静态编译进主程序，不依赖 `unrar.dll`。
- 枚举归档内部条目。
- 过滤目录条目。
- 读取条目压缩大小、原始大小、CRC、压缩方法、外部属性、修改时间。
- 按 ZIP UTF-8 标志位选择 UTF-8 或系统代码页做路径转换。
- 支持解压单个条目到用户选择目录。
- 7z 单条目解压已接入右键解压流程。
- RAR 单条目解压已接入右键解压流程。
- 7z 的 solid archive 多文件共享压缩块时，压缩后大小显示为 `-`，避免把共享块大小误认为逐文件压缩大小。
- RAR 加密、缺卷或超大字典场景当前会失败并写日志，不阻断其他归档索引。
- 对目录条目可创建目录。
- 解压时做 CRC 错误检查。

部分完成：

- `LibArchiveParser` 已有历史草稿，但 libarchive 在 list 模式下无法可靠获取条目的压缩后大小，不满足索引展示需求，因此该路线已放弃。

待完善：

- 继续寻找非 libarchive 的多格式方案，或按格式逐步实现专用解析器。
- 新解析方案必须能在列表模式获取条目的压缩后大小。
- UnRAR 源码只能用于处理 RAR，不能用于实现 RAR 压缩器或重建 RAR 压缩算法。
- 完善 ZIP 内部路径编码兼容。
- 解压时当前主要按 basename 输出，未保留完整内部目录结构。
- 损坏归档、加密归档、超长路径等场景需要更多处理。

### 5. 数据库模块

对应文件：

- `src/database.h`
- `src/database.cpp`

已完成：

- SQLite 打开和关闭封装。
- 文本统一以 UTF-8 写入 SQLite。
- 设置 WAL、同步级别、缓存、临时存储和 mmap。
- 创建 `configs`、`archives`、`entries` 表。
- 创建常用索引。
- 批量插入/更新归档记录。
- 批量插入条目记录。
- 按归档路径或归档 ID 删除条目。
- 按盘符 + FileReferenceNumber 查询/删除归档。
- 查询归档总数。
- 保存和读取每个盘符的 Journal 位置。
- 查询匹配过滤条件的归档或条目。
- 查询匹配条件下的 entry rowid 列表。
- 按 rowid 查询单条完整展示数据。
- 事务、回滚、提交、Vacuum 基础能力。

待完善：

- 搜索当前主要是 `LIKE`，大规模场景可考虑 FTS5。
- 排序中提取文件名的 SQL 表达式需要更多路径分隔符兼容验证。
- 数据库 schema 版本迁移机制尚未建立。

### 6. 主窗口与 UI 模块

对应文件：

- `src/main_window.h`
- `src/main_window.cpp`
- `src/resource.h`
- `src/strings.rc`

已完成：

- 创建主窗口、菜单栏、搜索框、虚拟 ListView、状态栏、Spinner。
- ListView 列：
  - 名称。
  - 归档文件。
  - 内部路径。
  - 压缩大小。
  - 原始大小。
- 搜索框输入 debounce，默认约 200ms 后刷新。
- 后台异步查询 rowid 列表，避免 UI 线程直接做大查询。
- 虚拟 ListView 按需取行。
- 列头点击排序。
- 列头显示升序/降序箭头。
- 搜索关键词在名称、归档路径、内部路径列中加粗显示。
- 状态栏显示处理状态、条目数量、归档文件数量。
- 后台索引运行时显示自绘 Spinner。
- DPI 缩放：按窗口 DPI 重建字体并布局控件。
- 系统语言资源表：中文和英文资源均已有。
- 窗口关闭时默认隐藏到托盘。

右键菜单已完成：

- 打开归档所在文件夹，并选中归档文件。
- 打开归档文件。
- 解压当前条目到用户选择目录。
- 复制文件名。
- 复制归档内路径。
- 复制归档文件路径。
- 打开归档文件属性对话框。

部分完成或占位：

- 双击当前仍是占位提示。
- “选项”菜单当前没有完整设置界面。
- “检查更新”尚未实现实际更新检查。
- “关于”仅为基础信息。

待完善：

- 资源文件中中文字符串需要重新检查编码和语法完整性。
- 右键解压可补充进度、打开输出目录等体验。
- 键盘快捷键和焦点行为可继续完善。

### 7. 行缓存与图标模块

对应文件：

- `src/row_cache.h`
- `src/row_cache.cpp`
- `src/icon_cache.h`
- `src/icon_cache.cpp`

已完成：

- `RowCache` 实现按 rowid 的 LRU 缓存。
- 默认最多缓存 2000 行。
- 缓存未命中时按 rowid 查询 SQLite。
- 将条目路径拆出文件名。
- 格式化压缩大小和原始大小。
- 与 `IconCache` 协作获取文件类型图标。
- `IconCache` 使用系统小图标 ImageList。
- 按扩展名缓存系统图标索引。

待完善：

- 可根据实际滚动行为调整缓存容量。
- 对无扩展名或特殊文件名的图标策略可继续细化。

### 8. 托盘模块

对应文件：

- `src/tray_icon.h`
- `src/tray_icon.cpp`

已完成：

- 添加托盘图标。
- 删除托盘图标。
- 托盘菜单支持显示窗口。
- 托盘菜单支持退出。
- 左键/双击托盘图标显示窗口。
- 关闭窗口默认隐藏到托盘，强制退出时销毁窗口。

待完善：

- 托盘图标当前使用默认系统图标。
- 可增加索引状态提示或右键菜单更多命令。

### 9. 配置模块

对应文件：

- `src/config/advconfig.h`
- `src/config/advconfig.cpp`
- `src/config/user_config.h`
- `src/config/user_config.cpp`

已完成：

- 自研配置解析器支持：
  - Null。
  - Bool。
  - Int。
  - Float。
  - String。
  - List。
  - Dict。
  - 注释。
  - 序列化。
  - UTF-8 BOM。
  - CRLF。
- `UserConfig` 封装应用配置。
- 配置不存在时自动写入默认配置。
- 当前支持 `archive_formats`。
- 当前支持 `scan_drives`，测试默认值为 `["G"]`；设置为空列表表示扫描所有 NTFS 盘。
- 兼容列表格式和旧的逗号分隔字符串格式。
- 扩展名自动补点并转小写。

待完善：

- UI 选项页尚未接入配置修改。
- 配置项范围仍少，尚未支持排除路径、启动行为、扫描策略等。

### 10. 字符串与日志工具

对应文件：

- `src/string_utils.h`
- `src/string_utils.cpp`
- `src/logger.h`
- `src/logger.cpp`

已完成：

- 宽字符串和 UTF-8 转换工具。
- 字符串小写转换。
- 条目路径取文件名。
- 文件大小格式化。
- 千位分隔格式化。
- 日志初始化、写入和关闭。

待完善：

- 日志轮转、日志级别配置、错误报告入口尚未产品化。

### 11. 测试与辅助程序

对应文件：

- `test/CMakeLists.txt`
- `test/test_zip.cpp`
- `test/test_usn_scan.cpp`
- `test/test_mft_scan.cpp`
- `test/test_file_tracker.cpp`
- `test/query_db.cpp`
- `test/test_config.cpp`
- `test/test_config.cfg`

已完成：

- CMake 已包含多个测试/辅助目标。
- ZIP 解析测试程序。
- USN 扫描测试程序。
- MFT 扫描测试程序。
- 文件追踪测试程序。
- 数据库查询工具。
- 配置解析器较完整的断言测试。

待完善：

- 尚未接入 CTest 或自动化测试流程。
- 主程序 UI 和索引器缺少自动化回归测试。
- ZIP 测试依赖本机固定路径，需要改成可移植测试数据。

## 当前可用功能清单

- 管理员权限启动。
- 单实例运行。
- 自动创建 `everyzip.db`。
- 自动创建 `everyzip.cfg`。
- 默认测试配置下扫描 G 盘；`scan_drives` 为空列表时扫描所有 NTFS 盘。
- 根据配置扩展名识别归档文件。
- 索引 ZIP/7z/RAR 内部文件条目。
- 搜索归档内部路径。
- 展示条目名称、归档路径、内部路径、压缩大小、原始大小。
- 按列排序。
- 匹配关键词加粗。
- 大结果集虚拟列表展示。
- 后台增量监控归档变化。
- 右键打开归档所在目录。
- 右键打开归档文件。
- 右键解压单个条目。
- 右键复制名称/路径。
- 右键查看属性。
- 最小化到系统托盘。

## 当前未完成或需要确认的功能

- 更多归档格式正式支持（不采用 libarchive，需要可返回压缩后大小的替代方案）。
- 设置窗口。
- 排除路径/盘符。
- 双击默认动作。
- 开机自启动。
- 自定义应用图标。
- 数据库 schema 迁移。
- 自动化测试。
- 发布安装包。

* 多选解压
* 搜索能力增强：
  * 通配符操作符等
  * 正则表达式搜索
* 更多语言

* 条目拖拽功能开发：
  * 拖拽到文件夹空白处：
    * 【名称】栏：解压到空白处所在的目录
    * 【归档文件】栏：复制到空白处所在的目录
  * 拖拽到xxx程序：
    * 【名称】栏：解压到临时目录，再用xxx程序打开
    * 【归档文件】栏：用xxx程序打开

* 文件统计面板

## ezdb v13 重构阶段记录

更新时间：2026-06-05。

当前状态：

- `ezdb_postings.c/.h` 已接管 postings builder、tokenizer、container encode、postings 写入、postings 读取和交集加载 helper。
- 旧 postings event spool sorter 死代码已删除。
- `ezdb_query.c/.h` 已新增，query AST/parser、通配符匹配、ASCII casefold contains、候选 key 构建和文本匹配 helper 已迁出 `ezdb.c`。
- `ezdb_format.c/.h` 已加入 v13 fixed header、section descriptor、section table 读写/校验/查找 helper。
- builder 已开始输出 `EZDB0013` v13 section directory 格式，v12 旧库打开会返回 format error。
- archive 级 delta insert/update/delete 和 compact 的 v13 smoke 已通过。

已验证：

- Debug `EzdbBench` 构建通过。
- v13 build/info/search smoke 曾通过。
- v13 section table 损坏、重复 section id、offset 越界均能返回 format error。
- v13 archive delta insert/update/delete 后 search smoke 通过。
- v13 compact smoke 通过。

下次继续的断点：

- `EzdbBench live-entry-append-batch test_data\sample_100_50k.combined.tsv test_data\codex_tmp\v13_live_entry_append.ezdb 256` 能写入完成。
- 随后 `EzdbBench info test_data\codex_tmp\v13_live_entry_append.ezdb` 仍返回 `open failed: invalid ezdb format (-3)`。
- 已用脚本确认该文件的 v13 header、section table 和 delta frame 顺序基本结构可解析，下一步应在 `ezdb_open()` / `replay_delta_log()` 内缩小具体 `EZDB_ERR_FORMAT` 返回点。
- 优先检查 entry append 回放后的 entry page size 校验、active entry count、delta entry path copy、`delta_entry_index_add_path()` 相关路径。

## 已完成
* 多线程解析压缩包
  * 3271 个文件
  * 单线程：57s
  * 2线程：32s
  * 4线程：22s
  * 6线程：18s

* 适配高分屏

* 检查更新

## 建议下一步

优先级建议如下：

1. 修复资源文件和 README 的中文编码显示问题，保证基础构建资源稳定。
2. 把右键菜单和双击行为打磨完整，形成可用 MVP。
3. 设置页已接入 `archive_formats` 可视化编辑，后续继续打磨更多格式支持。
4. 给索引器补恢复策略：Journal ID 变化时自动全量重建。
5. 沿格式专用解析器路线继续评估 tar.gz 等格式，候选方案必须能返回压缩后大小。
6. 将核心测试接入 CTest，并移除本机固定路径依赖。
