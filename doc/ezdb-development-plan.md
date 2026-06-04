# ezdb 自定义索引数据库需求与设计

## 1. 背景与目标

`ezdb` 是 EveryZip 面向“大量文件路径、归档文件和归档内部条目快速搜索”的自定义数据库格式。它不追求通用 SQL 能力，而是针对 EveryZip 的核心业务数据做紧凑存储、快速构建和低延迟搜索。

当前格式版本为 `EZDB0012`，不兼容 v1-v11。旧 `.ezdb` 文件需要重新构建。

核心目标：

- 文件扩展名为 `.ezdb`。
- 核心库使用 C 语言实现，在 Windows 上编译运行。
- 支持单项 archives 数据：本机压缩/归档文件。
- 支持双项 archives + entries 数据：归档文件及其内部文件条目。
- 支持 archive path、entry path、archive + entry 组合搜索。
- 支持 build、open、info、get、search、insert、update、delete。
- 支持事务批量写入和 `insert_many`。
- 支持压缩包 entries 边解析边入库，避免全量 entries 常驻内存后再一次性导入。
- 支持 entry delta 增量索引与后台 compact，使解析中 `.ezdb` 持续增长并可在解析后吸收到干净基座。
- 支持百万到千万级路径数据快速搜索，常见热查询目标 300ms 以内。
- 通过目录树、字符串复用、压缩 records、压缩 postings 降低磁盘占用。
- 打开后不常驻完整 path 数组，只按需重建返回结果路径。
- EveryZip 主程序运行时数据库使用 `everyzip.ezdb`；SQLite 仅保留为旧 `everyzip.db` 的一次性导入来源和对照测试后端。
- 首次启动没有 `everyzip.ezdb` 但存在 `everyzip.db` 时，自动导入到临时 `.ezdb.tmp`，校验后原子替换为正式 `.ezdb`。
- 主程序通过 `IndexStore` 抽象访问索引库，默认工厂创建 `EzdbIndexStore`。

## 2. 数据模型

### 2.1 Archives

每条 archive 记录对应 EveryZip `archives` 表的一行：

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `id` | `uint32_t` | ezdb 内部 archive id，从 0 开始。 |
| `drive_letter` | `char` | 盘符，例如 `C`。 |
| `file_ref_number` | `uint64_t` | NTFS file reference number。 |
| `usn` | `int64_t` | USN Journal 位置。 |
| `file_path` | UTF-8 字符串 | 归档文件完整路径。 |
| `file_size` | `uint64_t` | 文件字节大小。 |
| `modified_time` | `uint64_t` | 文件修改时间，原样保存调用方输入值。 |

archive path 仍拆成目录和文件名：

```text
C:\Program Files\App\bin\a.zip
dir  = C:\Program Files\App\bin
name = a.zip
```

构建阶段按目录树 DFS 顺序重排 archive id，使目录命中可扩展为连续 archive id range。

### 2.2 Entries

每条 entry 记录对应 EveryZip `entries` 表的一行：

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `id` | `uint32_t` | ezdb 内部 entry id，从 0 开始。 |
| `archive_id` | `uint32_t` | 关联的 ezdb archive id。 |
| `entry_path` | UTF-8 字符串 | 规范化后的归档内部条目路径。 |
| `entry_raw_path` | BLOB | 可选，仅非 UTF-8 原始条目路径需要原始字节定位时保存。 |
| `compressed_size` | `int64_t` | 压缩后大小，允许 `-1` 表示不可得。 |
| `original_size` | `uint64_t` | 原始大小。 |
| `modified_time` | `uint64_t` | 条目修改时间，原样保存调用方输入值。 |

删除 archive 时，这个 archive 对应的 entries 在搜索和读取语义上级联失效。

## 3. 文件格式

`.ezdb` 使用固定文件头加分段 section：

```text
+----------------------+
| EzdbHeader           |
+----------------------+
| archive file records |
+----------------------+
| archive metadata     |
+----------------------+
| dir records          |
+----------------------+
| string pool          |
+----------------------+
| archive postings     |
+----------------------+
| archive indexes      |
+----------------------+
| entry core records   |
+----------------------+
| entry detail pages   |
+----------------------+
| entry raw/blob pages |
+----------------------+
| entry postings       |
+----------------------+
| entry index          |
+----------------------+
| append delta log     |
+----------------------+
```

文件头保存：

- magic：固定为 `EZDB0012`。
- `file_count` / `active_count`：archive 逻辑记录数和活跃数。
- `entry_count` / `active_entry_count`：entry 逻辑记录数和活跃数。
- `base_entry_count`：压缩基座中的 entry 数量；大于该值的 active entry 来自 delta log。
- archive records、archive metadata、dir records、string pool 的 offset/size/raw_size/flags。
- archive file index、archive dir index、entry index 的 offset/count。
- postings section 的 offset/size，entry postings 追加在同一 postings 基址后。
- entry core、entry detail page index、raw/blob page index 的 offset/size/count/flags。
- delta offset/size：append-only archive CRUD、entry replace/append 日志位置和大小。

### 3.1 Archive Records

archive path records 使用 compact varint 磁盘编码，section 可选 zlib 压缩。打开数据库时通过 zlib 流式解码成列式数组：

```text
parent_dir_id[]        uint32_t
name_offset[]          uint32_t
name_len[]             uint16_t
size32[]               uint32_t，超过 32 位时进入 overflow 表
modified_time32[]      uint32_t，超过 32 位时进入 overflow 表
archive_meta[]         file_ref_number + usn + drive_letter
```

### 3.2 Entries

entries 拆成打开阶段常驻的轻量 core、按需读取的 detail pages、按需读取的 blob pages：

```text
entry core:
  archive_id
  entry_path_offset / entry_path_len

entry detail page:
  archive_id
  entry_path_offset / entry_path_len
  raw_offset / raw_len
  compressed_size
  original_size
  modified_time

entry raw/blob page:
  entry_path bytes + optional entry_raw_path bytes
```

`entry_path` 和 `entry_raw_path` 都保存在逻辑 raw/blob 空间中，磁盘上按 256 KiB 原始页独立压缩。搜索最终确认和 `get-entry` 只加载命中的少数 blob page；entry detail page 默认按 4096 条 entry 独立压缩，只有返回结果或回读 entry 时才加载。

v12 中，新解析出来的 entries 可以先写入 delta entry frame。delta entry 的 path/raw bytes 保存在 delta frame 内，打开或追加时建立轻量引用；后台 compact 后再重新写入 entry core、detail pages、raw/blob pages 和压缩 entry postings。打开和在线追加时维护 archive -> entry 的内存 ownership 链表，使 archive entry replace/delete 不再扫描全量 entries。

### 3.3 Index And Postings

搜索索引拆成三类：

- archive 文件名 gram 索引：gram -> archive id postings。
- archive 目录组件 gram 索引：gram -> dir id postings，再扩展为 archive id range。
- entry path gram 索引：gram -> entry id postings。
- delta entry path 内存 gram 索引：解析追加或 replay delta 时建立，查询时与压缩基座 entry index 合并。

索引 entry：

```c
typedef struct EzdbDiskIndex {
    uint32_t key;
    uint32_t count;
    uint32_t container_type;
    uint32_t encoded_size;
    uint32_t raw_size;
    uint64_t offset;
} EzdbDiskIndex;
```

gram 以 UTF-8 token 为单位生成：

- ASCII 字符按单字节 token 处理，并做 ASCII casefold。
- 非 ASCII 字符按完整 UTF-8 codepoint token 处理，支持中文路径搜索。
- 1/2/3 token gram 同时用于 archive 和 entry 索引。

postings 使用自适应容器：

- array container：低频或稀疏 id 使用 delta varint。
- range container：连续或近连续 id 使用 `start + len` run 编码。
- bitset container：高频稠密 id 使用 bitset。

每个 postings container 会按收益独立选择 zlib 压缩。查询时只读取命中的少数 container，不加载整个 postings section。

## 4. 搜索设计

搜索语义支持查询表达式。`ezdb_search_path` 保留为 archive-only 兼容入口；新增 `ezdb_search` 支持 scope 参数。

scope：

| Scope | 说明 |
| --- | --- |
| `EZDB_SEARCH_ARCHIVE_PATH` | 只搜索 `archives.file_path`。 |
| `EZDB_SEARCH_ENTRY_PATH` | 只搜索 `entries.entry_path`。 |
| `EZDB_SEARCH_COMBINED_PATH` | 搜索 archive path + entry path 的组合。 |
| `EZDB_SEARCH_ALL` | archive-only 结果 + entry-only 结果。 |

支持的查询语法：

| 语法 | 作用 | 示例 |
| --- | --- | --- |
| `关键词` | 普通包含搜索 | `发票` |
| `关键词1 关键词2` | AND，同时包含多个词 | `合同 甲方` |
| `"短语"` | 连续短语或字面特殊符号 | `"用户协议"`、`"a*b"` |
| `*` | 匹配任意长度 UTF-8 字符序列 | `张*明` |
| `?` | 匹配单个 UTF-8 字符 | `第?章` |
| `!关键词` | 排除关键词 | `合同 !草稿` |
| `词1 | 词2` | OR，任意一个匹配即可 | `发票 | 收据` |
| `( ... )` | 分组组合条件 | `(发票 | 收据) 报销` |

查询流程：

```text
输入 keyword + scope
  |
  +-- 解析为查询 AST
  +-- 从正向 term/phrase/wildcard 中提取可索引 literal
  +-- archive scope：复用 file index + dir index 生成 archive 候选 bitset
  +-- entry scope：复用 base entry index + delta entry index 生成 entry 候选 bitset
  +-- combined scope：entry index 候选 + archive 命中扩展到 active entries
  +-- AND 分支取交集，OR 分支取并集，NOT 只做最终过滤
  +-- 没有正向 literal 时允许全量扫描，并在 limit > 0 时提前停止
  +-- 对候选完整字符串做 AST 最终匹配
  +-- 只为最终结果构造路径并 callback
```

combined scope 中，每个 term/wildcard 可以命中 archive path 或 entry path；最终匹配使用 `archive_path + '\n' + entry_path`，不会把短语跨字段边界当作连续路径。

## 5. 主程序集成与 CRUD 设计

### 5.1 IndexStore 集成

EveryZip 主程序不再直接依赖 `Database` 执行运行时查询和写入，而是通过 `IndexStore` 抽象访问索引库。

核心约定：

- `CreateIndexStore()` 默认返回 `EzdbIndexStore`。
- `CreateSQLiteIndexStore()` 保留给对照测试和旧库迁移验证。
- 主程序默认数据库路径为可执行文件目录下的 `everyzip.ezdb`。
- `StoreEntryId` 使用 `int64_t`，`kInvalidStoreEntryId = -1`；UI 和 row cache 不再假设有效 entry id 必须大于 0。
- `RowCache`、主窗口异步加载、右键菜单、tooltip、虚拟列表回读都通过 `IndexStore` 查询。
- `Indexer` 的全量扫描和 USN 增量监控通过 `IndexStore` upsert archive、替换 entries、保存 journal/config meta。

`EzdbIndexStore::OpenOrCreate` 行为：

```text
打开 everyzip.ezdb
  |
  +-- 文件存在：直接 ezdb_open
  |
  +-- 文件不存在：
        |
        +-- 同目录 everyzip.db 存在：读取 SQLite archives/entries/configs，构建 everyzip.ezdb.tmp，校验 count 后原子替换
        |
        +-- everyzip.db 不存在：构建空 everyzip.ezdb
```

SQLite 导入时：

- `archives` 和 `entries` 转成 `EzdbArchiveRecord` / `EzdbEntryRecord`。
- SQLite archive id 重映射为 ezdb archive id。
- `configs` 中的 journal cursor 保持 `journal_id_X` / `next_usn_X` key。
- 普通配置写入 ezdb meta 时加 `config_` 前缀，匹配 `EzdbIndexStore::GetConfigValue`。

当前 meta 存储在旁路 `everyzip.ezdb.meta` 文件中。显式写事务内的 meta 写入会先缓冲，`CommitWrite` 成功后再落盘；这保证 rollback 不会提前污染 meta，但它还不是嵌入 `.ezdb` 主文件的单文件事务。

### 5.2 ezdb CRUD

v12 延续“压缩基座 + append-only delta log + write transaction”，并把 archive entries 替换升级为 C 层增量写入：

- build 完成后的 records、dirs、strings、indexes、postings 作为不可变压缩基座。
- archive insert/update/delete 追加 delta log，不在基座中间移动数据。
- 单条写入默认保持“调用成功返回前持久化”。
- 批量写入通过事务延迟 flush，commit 时一次性写 batch frame、更新 header 并落盘；entry replace/append/finish 在事务内不再逐批 `_commit`。
- open 时 replay 已提交 delta batch，构建 id 覆盖层和 tombstone。
- `ezdb_delete_archive_by_ref` 根据 `drive_letter + file_ref_number` 删除 archive，并让其 entries 级联失效。
- `ezdb_begin_replace_archive_entries` 先写入 archive entry delete delta，旧 entries 通过 ownership 链表按 archive 失效。
- `ezdb_append_archive_entries` 按批追加 delta entry frame，并同步维护 active entries、delta entry refs 和 delta entry gram index。
- `ezdb_finish_replace_archive_entries` 完成该 archive 的 entry 替换；`ezdb_abort_replace_archive_entries` 用于失败时终止未完成批次。
- `ezdb_replace_archive_entries` 作为一次性包装保留，内部走 begin / append / finish。
- `ezdb_compact` 流式读取当前 active archives / active entries，生成新的压缩快照并原子替换当前 `.ezdb`，compact 后 delta size 和 delta entry count 归零。

已实现：

- `ezdb_insert`
- `ezdb_update`
- `ezdb_delete`
- `ezdb_upsert_archive`
- `ezdb_upsert_archives`
- `ezdb_delete_archive_by_ref`
- `ezdb_get_archive_by_ref`
- `ezdb_query_entries`
- `ezdb_get_entries_batch`
- `ezdb_get_meta`
- `ezdb_put_meta`
- `ezdb_begin_write`
- `ezdb_commit_write`
- `ezdb_rollback_write`
- `ezdb_insert_many`
- `ezdb_begin_replace_archive_entries`
- `ezdb_append_archive_entries`
- `ezdb_finish_replace_archive_entries`
- `ezdb_abort_replace_archive_entries`
- `ezdb_replace_archive_entries`
- `ezdb_compact`

EveryZip 的解析流程使用 parser `ForEachEntry` 回调边解析边攒小批量 entries，`Indexer` 再通过 `IndexStore` batch entry replacement 写入 ezdb。首次扫描不再把整个压缩包 entry 列表放入内存后统一导入，也不再在全量解析期间长期持有全局写事务；因此 `everyzip.ezdb` 会在解析过程中持续增长，解析结束或监控变更后由后台 compact 吸收 delta。

## 6. 内存设计

打开数据库时加载：

- archive compact records 解码后的列式数组。
- archive metadata。
- entry core 列式数组：`archive_id`、`entry_path_offset`、`entry_path_len`。
- entry detail page index 和 raw/blob page index。
- 目录记录表。
- 字符串池。
- archive 文件名索引、archive 目录组件索引、entry path 索引。
- delta 覆盖层、active bitset、covered_base_bits。
- delta entry refs、delta entry path/raw arena 和 delta entry 内存 gram 索引。

按需读取：

- postings container。
- entry detail pages：返回结果或 `get-entry` 需要 size/mtime/raw offset 时加载，LRU 缓存最近页。
- raw/blob pages：候选确认、返回 entry path 或 raw bytes 时加载，LRU 缓存最近页。
- 返回结果对应的 archive path、entry path、raw bytes 副本。

设计原则：

- 不常驻完整 archive path 数组。
- 不常驻完整 entry detail 记录和 entry path/raw blob。
- 保留 entry gram/postings 热索引常驻，优先保护查询延迟。
- 搜索候选去重使用 bitset。
- 查询只为最终结果构造路径和结果对象。
- entry raw bytes 只在 `get-entry` 或结果需要时复制给调用方。
- 解析导入阶段只保留当前 parser batch、当前写入 batch 和 ezdb 必要覆盖层，不保留全量 archive entries vector。
- compact 后 delta entry refs 和 delta entry 内存索引被压缩基座替代，降低 reopen 成本和常驻内存。

## 7. 空间设计

当前主要空间来源：

- records：compact varint archive path 记录。
- archive metadata：盘符、file reference number、USN。
- dirs：archive 目录记录和目录组件。
- names：archive 文件名与目录组件字符串池。
- entry core：打开阶段常驻的轻量 entry 列。
- entry detail pages：按 entry id 分页压缩的完整 entry 记录。
- raw/blob pages：按原始 256 KiB 页独立压缩的 entry_path 和 entry_raw_path。
- postings：archive 文件名、archive 目录组件、entry_path 的 1/2/3-gram 倒排表。
- indexes：gram 到 postings 的映射。
- delta：在线增删改日志。

已完成的空间优化：

- archive 完整路径改为目录树 + 文件名存储。
- archive id 按目录树 DFS 顺序分配，目录命中转换为 archive id range。
- archive records 使用 compact varint 和 zlib section 压缩。
- entry core 使用独立 zlib section 压缩。
- entry detail 与 raw/blob pool 使用 page 化 zlib 压缩，打开时只读页目录。
- postings 使用 array/range/bitset 自适应容器，并按 container 独立压缩。
- open 阶段 records 流式解压成列式数组，降低临时内存和常驻内存。
- entry delta 支持边解析边追加，解析中只产生 append-only delta log。
- compact 已支持把 active archives/entries 流式重写为干净基座，吸收 tombstone、stale 版本和 entry delta。

后续空间优化方向：

- entry path/raw blob 增加字符串复用，降低 v9 page 化后的磁盘增量。
- size/mtime 使用 block base-delta 或按页懒加载。
- 字符串池按页加载或增加块压缩。
- 高频 postings 支持更细粒度懒展开。
- delta log 增加 CRC 和截断恢复，避免崩溃后保留半截 frame。

## 8. 性能验证

### 8.1 目标

- 100k archive-only 样本用于快速功能回归。
- 3200k archive-only 样本用于大数据量构建和搜索性能验证。
- `everyzip.db` SQLite 样本用于 archives + entries 双项导入和搜索验证。
- 常见 archive、entry、combined 热查询目标 300ms 以内。

### 8.2 工具

`EzdbBench`：

```text
EzdbBench build <input.txt> <output.ezdb>
EzdbBench build-archives <input.tsv> <output.ezdb>
EzdbBench info <db.ezdb>
EzdbBench get <db.ezdb> <id>
EzdbBench get-archive <db.ezdb> <id>
EzdbBench get-entry <db.ezdb> <id>
EzdbBench search <db.ezdb> <keyword> [limit]
EzdbBench search-v2 <db.ezdb> <archive|entry|combined|all> <keyword> [limit]
EzdbBench open <db.ezdb> [limit]
EzdbBench insert <db.ezdb> <path> [size] [mtime]
EzdbBench insert-file <db.ezdb> <input.txt>
EzdbBench update <db.ezdb> <id> <path> [size] [mtime]
EzdbBench delete <db.ezdb> <id>
EzdbBench delete-archive-ref <db.ezdb> <drive> <file_ref_number>
EzdbBench crud <input.txt> <output.ezdb>
EzdbBench build-entries <combined.tsv> <output.ezdb>
EzdbBench live-entry-append <output.ezdb> <entry_count> [batch_size]
EzdbBench live-entry-append-batch <combined.tsv> <output.ezdb> [batch_size]
EzdbBench compact <db.ezdb>
```

合并 TSV 导入测速：

```text
python tools\generate_entry_sample.py --output test_data\sample_10k_3m.combined.tsv --archive-count 10000 --entry-count 3000000 --entries-max 500
EzdbBench build-entries test_data\sample_10k_3m.combined.tsv test_data\bench_10k_3m_build.ezdb
EzdbBench live-entry-append-batch test_data\sample_10k_3m.combined.tsv test_data\bench_10k_3m_live.ezdb 4096
EzdbBench info test_data\bench_10k_3m_build.ezdb
EzdbBench info test_data\bench_10k_3m_live.ezdb
```

记录 `build_entries_ms`、`live_batch_append_ms`、输出文件大小和 peak working set；`info` 需确认 `records=10000`、`entries=3000000`。

查询语法回归脚本：

```text
python tools\ezdb_query_syntax_tests.py --bench cmake-build-codex-release\tools\Release\EzdbBench.exe
```

### 8.3 当前 v8 Release 结果

#### 100k archive-only

测试输入：`test_data\list_files_100k.tsv`  
输出：`test_data\list_files_100k_v8_entryidx.ezdb`

| 指标 | 数值 |
| --- | ---: |
| archives | 100,000 |
| entries | 0 |
| 构建耗时 | 631 ms |
| 文件大小 | 9,555,967 bytes / 9.11 MB |
| 构建峰值 working set | 47.34 MB |
| info private | 11.10 MB |

#### 3200k archive-only

测试输入：`test_data\list_files_3200k.tsv`  
输出：`test_data\list_files_3200k_v8_entryidx.ezdb`

| 指标 | 数值 |
| --- | ---: |
| archives | 3,221,428 |
| entries | 0 |
| 构建耗时 | 14,488 ms / 14.49s |
| 文件大小 | 151,120,729 bytes / 144.12 MB |
| 构建峰值 working set | 930.15 MB |
| open | 426-436 ms |
| info private | 197.41 MB |

抽测查询：

| scope | keyword | limit | search_ms | returned |
| --- | --- | ---: | ---: | ---: |
| archive | `index` | 20 | 4 | 20 |
| archive | `zzznotfoundzzz` | 20 | 0 | 0 |
| archive | `a` | 20 | 29 | 20 |

#### everyzip.db 双项导入

测试输入：`test_data\everyzip.db`  
输出：`test_data\everyzip_v8_entryidx.ezdb`

| 指标 | 数值 |
| --- | ---: |
| archives | 3,476 |
| entries | 547,902 |
| SQLite 读取耗时 | 157 ms |
| ezdb 构建耗时 | 2,094 ms |
| 文件大小 | 21,532,464 bytes / 20.54 MB |
| 构建峰值 working set | 365.23 MB |
| info private | 54.17 MB |
| `entry_raw_path` 回读 | 已验证，示例 raw=12 |

抽测查询：

| scope | keyword | limit | search_ms | returned |
| --- | --- | ---: | ---: | ---: |
| entry | `kwpsaitablestyle` | 5 | 1 | 5 |
| combined | `download.7z diff_resource_file` | 5 | 4 | 5 |
| combined | `kwpsaitablestyle diff_resource_file` | 5 | 4 | 1 |
| entry | `zzznotfoundzzz` | 5 | 0 | 0 |

级联删除验证：

- 对 `test_data\everyzip_v8_entryidx_delete_test.ezdb` 删除 archive id `3474` 后，顺序执行 combined 查询 `kwpsaitablestyle diff_resource_file` 返回 0。

查询语法回归：

- `tools\ezdb_query_syntax_tests.py` 全部通过。

### 8.4 当前 v9 Release 结果

测试输入：`test_data\everyzip_300k_5m.db`

输出：`test_data\everyzip_300k_5m_import_v9_mem.ezdb`

| 指标 | 数值 |
| --- | ---: |
| archives | 300,000 |
| entries | 5,000,000 |
| 文件大小 | 110,436,456 bytes / 105.32 MB |
| info working set | 88.18 MB |
| info peak working set | 144.64 MB |
| info private | 85.30 MB |
| entry core section | 25,918,964 bytes / 24.72 MB |
| raw/blob pages | 15,562,920 bytes / 14.84 MB |
| index section | 114,816 bytes / 0.11 MB |
| postings section | 1,372,090 bytes / 1.31 MB |

相对 `everyzip_300k_5m_import_v8_bench.ezdb`：

| 指标 | v8 | v9 |
| --- | ---: | ---: |
| 文件大小 | 80.38 MB | 105.32 MB |
| info private | 504.76 MB | 85.30 MB |
| info peak working set | 520.25 MB | 144.64 MB |
| entry `index` search | 19 ms | 14 ms |
| combined `download diff_resource_file` search | 147 ms | 182 ms |
| combined `kwpsaitablestyle diff_resource_file` search | 176 ms | 170 ms |

回读验证：

| 命令 | 结果 | private |
| --- | --- | ---: |
| `get-archive 0` | 成功 | 85.31 MB |
| `get-entry 0` | 成功，包含 raw path | 86.52 MB |
| `get-entry 4999999` | 成功 | 85.63 MB |

级联删除验证：

- 临时副本删除 archive ref `C + 10000000000` 后，combined 查询 `kwpsaitablestyle_0000000` 返回 0。

### 8.5 当前 v10 Release 结果

测试输入：`test_data\everyzip_300k_5m.db`

输出：`test_data\everyzip_300k_5m_v10_codex.ezdb`

| 指标 | 数值 |
| --- | ---: |
| archives | 300,000 |
| entries | 5,000,000 |
| SQLite 导入墙钟 | 36,069 ms |
| 输出文件大小 | 110,436,456 bytes / 105.32 MB |
| info working set | 88.11 MB |
| info peak working set | 144.56 MB |
| info private | 85.31 MB |
| entry core section | 25,918,964 bytes / 24.72 MB |
| raw/blob pages | 15,562,920 bytes / 14.84 MB |
| index section | 114,816 bytes / 0.11 MB |
| postings section | 1,372,090 bytes / 1.31 MB |

旧 SQLite 对照数据不再通过 `EzdbBench` 直接导入；archives + entries 性能验证统一使用合并 TSV 样本。

抽测查询：

| scope | keyword | limit | search_ms | returned | private |
| --- | --- | ---: | ---: | ---: | ---: |
| entry | `index` | 20 | 12 | 20 | 85.70 MB |
| combined | `index` | 20 | 13 | 20 | 85.71 MB |
| entry | `download diff_resource_file` | 20 | 132 | 20 | 91.78 MB |
| entry | `download diff_resource_file` | 0 | 293 | 100 | 104.79 MB |
| entry | `no_such_keyword_zzzz_codex` | 0 | 1 | 0 | 85.24 MB |

说明：

- v10 文件体积和运行时 `info_private_mb` 与 v9 大样本基线基本持平。
- `index limit=0` 会返回 500,000 条，实测 `1457 ms`，主要成本是候选确认和结果枚举，不代表普通热查询退化。
- `TestIndexStore` 覆盖 SQLiteIndexStore、EzdbIndexStore 和旧 SQLite 自动导入路径。
- `tools\ezdb_query_syntax_tests.py` 会在缺少本地样本文件时自动生成小样本，避免 `test_data\files_query_syntax.txt` 被 `.gitignore` 排除导致回归脚本失效。

### 8.6 当前 v11 streaming entry delta / compact 结果

v11 的重点验证是“entries 边解析边入库”和“解析后 compact 吸收 delta”。本轮实现验证命令：

```text
cmake --build cmake-build-codex-release --config Release --target EveryZip TestIndexStore EzdbBench
ctest --test-dir cmake-build-codex-release -C Release --output-on-failure
python tools\ezdb_query_syntax_tests.py --bench cmake-build-codex-release\tools\Release\EzdbBench.exe
```

验证结果均通过。

100k live append smoke：

```text
EzdbBench live-entry-append test_data\codex_tmp\live_append_100k_final.ezdb 100000 4096
```

| 阶段 | 指标 | 数值 |
| --- | --- | ---: |
| append 后 | 文件大小 | 7,702,445 bytes / 7.35 MB |
| append 后 | delta_entries | 100,000 |
| append 后 | delta_size | 7,700,032 bytes / 7.34 MB |
| append 后 | append_ms | 2,936 ms |
| append 后 | `live_file_00099999` 查询 | 3 ms |
| append 后 | info private | 47.17 MB |
| compact | compact_ms | 1,689 ms |
| compact 后 | 文件大小 | 1,463,971 bytes / 1.40 MB |
| compact 后 | base_entries | 100,000 |
| compact 后 | delta_entries / delta_size | 0 / 0 |
| compact 后 | reopen open_ms | 3 ms |
| compact 后 | `live_file_00099999` 查询 | 9 ms |
| compact 后 | search private | 8.74 MB |

10k quick smoke：

| 阶段 | 指标 | 数值 |
| --- | --- | ---: |
| append 后 | append_ms | 156 ms |
| append 后 | 文件大小 | 772,445 bytes / 0.74 MB |
| append 后 | 查询 | 1 ms |
| compact | compact_ms | 190 ms |
| compact 后 | 文件大小 | 245,037 bytes / 0.23 MB |
| compact 后 | base_entries | 10,000 |
| compact 后 | delta_entries / delta_size | 0 / 0 |
| compact 后 | info private | 1.96 MB |

结论：

- parser / Indexer / EzdbIndexStore 已支持 entries 批量流式写入，解析过程中 `.ezdb` 不再长期停留在 1 KB。
- delta entry 查询路径可在 compact 前命中新增 entries；compact 后 entries 被吸收到压缩基座。
- compact 后 reopen 成本、常驻内存和文件体积明显下降；后续重点转向崩溃恢复、compact 触发阈值和 UI 查询分页。

### 8.7 当前 v12 entry ingest flush / ownership 优化结果

v12 的重点验证是“降低在线 entry delta 入库开销”：entry replace/append/finish 在写事务内延迟 header flush，并在内存中维护 archive -> entry ownership 链表，使 archive entries 替换和删除不再全库扫描 entries。

验证命令：

```text
cmake --build cmake-build-codex-release --config Release --target EzdbBench TestIndexStore
ctest --test-dir cmake-build-codex-release -C Release --output-on-failure
python tools\ezdb_query_syntax_tests.py --bench cmake-build-codex-release\tools\EzdbBench.exe
EzdbBench live-entry-append test_data\codex_tmp\live_append_v12_txn_100k.ezdb 100000 4096
EzdbBench compact test_data\codex_tmp\live_append_v12_txn_100k.ezdb
EzdbBench search-v2 test_data\codex_tmp\live_append_v12_txn_100k.ezdb entry live_file_00099999 5
```

100k live append smoke：

| 阶段 | 指标 | 数值 |
| --- | --- | ---: |
| append 后 | append_ms | 1,080 ms |
| append 后 | 文件大小 | 7,702,509 bytes / 7.35 MB |
| append 后 | delta_entries | 100,000 |
| append 后 | delta_size | 7,700,096 bytes / 7.34 MB |
| compact | compact_ms | 522 ms |
| compact 后 | 文件大小 | 1,463,971 bytes / 1.40 MB |
| compact 后 | base_entries | 100,000 |
| compact 后 | delta_entries / delta_size | 0 / 0 |
| compact 后 | reopen open_ms | 2 ms |
| compact 后 | `live_file_00099999` 查询 | 5 ms |
| compact 后 | info private | 8.22 MB |

结论：

- 相同 100k append smoke 相对 v11 `2,936 ms` 降至 `1,080 ms`，约 2.7 倍提升。
- 初始解析路径通过 `IndexStore::BeginWrite` / `CommitWrite` 包裹 archive entry append，可避免每个 archive begin/finish 和每个 append batch 都强制刷盘。
- ownership 链表把 archive entry replace/delete 的旧 entries 失效从全量扫描改为按 archive 遍历，避免初始解析时随 entry_count 增长出现累积扫描成本。

## 9. C API

公开接口位于 `src/ezdb/ezdb.h`。

```c
int ezdb_build_snapshot(const EzdbArchiveRecord* archives,
                        uint32_t archive_count,
                        const EzdbEntryRecord* entries,
                        uint32_t entry_count,
                        const char* output_ezdb);

int ezdb_build_snapshot_stream_entries(const EzdbArchiveRecord* archives,
                                       uint32_t archive_count,
                                       EzdbEntryStream* entry_stream,
                                       const char* output_ezdb);

int ezdb_open(const char* path, Ezdb** out_db);
void ezdb_close(Ezdb* db);

uint32_t ezdb_archive_count(Ezdb* db);
uint32_t ezdb_active_archive_count(Ezdb* db);
uint32_t ezdb_entry_count(Ezdb* db);
uint32_t ezdb_active_entry_count(Ezdb* db);
uint64_t ezdb_file_size(Ezdb* db);
int ezdb_stats(Ezdb* db, EzdbStats* out_stats);

int ezdb_get_archive(Ezdb* db, uint32_t id, EzdbArchiveResult* out_result);
int ezdb_get_entry(Ezdb* db, uint32_t id, EzdbEntryResult* out_result);
int ezdb_get_archive_by_ref(Ezdb* db, char drive_letter, uint64_t file_ref_number, EzdbArchiveResult* out_result);
void ezdb_free_archive_result(EzdbArchiveResult* result);
void ezdb_free_entry_result(EzdbEntryResult* result);
void ezdb_free_entry_query_page(EzdbEntryQueryPage* page);

int ezdb_search(Ezdb* db,
                const char* keyword,
                uint32_t scope,
                uint32_t limit,
                EzdbSearchV2Callback callback,
                void* user_data);

int ezdb_search_path(Ezdb* db,
                     const char* keyword,
                     uint32_t limit,
                     EzdbSearchCallback callback,
                     void* user_data);

int ezdb_query_entries(Ezdb* db, const EzdbEntryQuery* query, EzdbEntryQueryPage* out_page);
int ezdb_get_entries_batch(Ezdb* db, const uint32_t* ids, uint32_t count, EzdbEntryResult* out_results);

int ezdb_upsert_archive(Ezdb* db, const EzdbArchiveRecord* record, uint32_t* out_id);
int ezdb_upsert_archives(Ezdb* db, const EzdbArchiveRecord* records, uint32_t count, uint32_t* out_ids);
int ezdb_delete_archive_by_ref(Ezdb* db, char drive_letter, uint64_t file_ref_number);
int ezdb_begin_replace_archive_entries(Ezdb* db, uint32_t archive_id);
int ezdb_append_archive_entries(Ezdb* db, uint32_t archive_id, const EzdbEntryRecord* entries, uint32_t entry_count);
int ezdb_finish_replace_archive_entries(Ezdb* db, uint32_t archive_id);
int ezdb_abort_replace_archive_entries(Ezdb* db, uint32_t archive_id);
int ezdb_replace_archive_entries(Ezdb* db, uint32_t archive_id, const EzdbEntryRecord* entries, uint32_t entry_count);

int ezdb_begin_write(Ezdb* db, uint32_t flags);
int ezdb_commit_write(Ezdb* db);
int ezdb_rollback_write(Ezdb* db);
int ezdb_insert_many(Ezdb* db, const EzdbFileRecord* records, uint32_t count, uint32_t* first_id);

int ezdb_insert(Ezdb* db, const EzdbFileRecord* record, uint32_t* out_id);
int ezdb_update(Ezdb* db, uint32_t id, const EzdbFileRecord* record);
int ezdb_delete(Ezdb* db, uint32_t id);

int ezdb_get_meta(Ezdb* db, const char* key, char** out_value);
int ezdb_put_meta(Ezdb* db, const char* key, const char* value);
int ezdb_compact(Ezdb* db);

const char* ezdb_error_message(int code);
```

`build` / `build-archives` 是工具层测试入口：文本和 TSV 解析只保留在 `EzdbBench`，核心 `ezdb` 库只接收结构化 `EzdbArchiveRecord` / `EzdbEntryRecord` 快照输入。

API 约定：

- 返回值使用整数错误码。
- 查询结果字符串和 raw bytes 由 `ezdb` 分配，调用方使用对应 free API 释放。
- 搜索使用 callback，避免一次性分配巨大结果数组。
- `ezdb_search_path` 是兼容旧 path-only 使用方式的 archive-only 包装。
- `ezdb_query_entries` 返回 entry id page 和 total count；主程序排序分页由 `EzdbIndexStore` 继续封装。
- `ezdb_get_entries_batch` 用于批量回读 UI 当前页或 row cache 命中的 entry。
- `ezdb_build_snapshot_stream_entries` 允许调用方通过 `EzdbEntryStream` 边读取 entries 边构建 snapshot，避免构建阶段持有全量 entry 数组。
- entry replacement 支持 begin / append / finish / abort；EveryZip 解析压缩包时按 batch append。
- `ezdb_stats` 返回 base / delta entry 数量和 delta size，用于观察 compact 前后的状态。
- 单条写入成功返回前已持久化；事务内写入延迟到 `ezdb_commit_write`。
- 只读打开时写接口返回 `EZDB_ERR_READ_ONLY`。

## 10. 编码与兼容性

- 输入文本按 UTF-8 读取。
- `.ezdb` 内部路径字符串按 UTF-8 保存。
- `EzdbBench` 在 Windows 下使用宽字符命令行转 UTF-8，避免中文参数被本地代码页破坏。
- 当前索引按 UTF-8 token 建 1/2/3-gram，中文路径可以被索引搜索。
- 当前大小写折叠只处理 ASCII。
- `?` 通配符按 UTF-8 字符匹配，一个中文字符算一个字符。
- `modified_time` 不做 epoch/FILETIME 转换，调用方传入什么就保存什么。
- `entry_raw_path` 只保存和回传，不参与搜索。
- 完整 Unicode casefold 暂不作为当前目标，后续如有需要再引入 Unicode 规范化表或额外 normalized key。

## 11. 已知限制

- v12 不兼容 v1-v11 `.ezdb` 文件。
- 目录记录、字符串池、entry core 和索引仍在 open 后整体常驻。
- entry detail 与 raw/blob pool 已按页懒加载，但 entry path 当前没有目录树压缩，主要依赖 page 压缩和 postings 索引。
- 目录索引基于路径组件，不为任意跨组件子串单独建索引。
- 高频 1 字节关键词可能返回接近全库，`limit=0` 耗时主要由候选确认和结果回调决定。
- 纯排除和纯通配符查询允许全库扫描，`limit=0` 在大库上可能明显慢于普通索引查询。
- batch frame 具备 committed replay 语义，但还没有 CRC、自动截断不完整 delta 和并发读写控制。
- compact 前大规模 delta 仍会增加磁盘占用和 reopen 后 delta replay 成本；后台 compact 已接入，但极端中断或频繁变化场景仍需要更稳的触发阈值和恢复策略。
- meta 仍保存为旁路 `.meta` 文件，不是嵌入 `.ezdb` 主文件的单文件事务区。
- UI 当前仍主要加载完整 entry id 列表；`IndexStore::QueryEntriesPage` 已具备分页接口，但主窗口还没有完全切到查询会话/分页模型。

## 12. 后续迭代路线

优先级从高到低：

1. 增强崩溃恢复：batch CRC、打开时截断半截 frame、写入错误回滚。
2. 优化后台 compact 触发策略：按 delta size、delta entry count、空闲时间和监控变更频率动态决策。
3. 将 meta 嵌入 `.ezdb` 或设计同目录事务文件，避免 `.meta` 旁路文件与主库提交点分离。
4. 推进 UI 查询会话/分页模型，减少主窗口一次性持有全部 entry id。
5. 优化大 delta 覆盖层内存：delta path arena 压缩、按页加载或独立 delta 索引。
6. 优化 entry path/raw blob：字符串复用、更细粒度页策略或 mmap。
7. 优化高频短词 `limit > 0` 的候选展开，尽早停止 range/bitset 扫描。
8. 评估 Windows mmap，让 records、strings、postings 支持更细粒度懒加载。
