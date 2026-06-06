# ezdb v13 提速与模块化重构计划

## 目标

ezdb v13 的目标是为 `build-zip-entries` 建立一个更快、更低内存、更容易维护的构建与查询内核。v13 不要求兼容旧 ezdb 文件格式，可以重新设计磁盘布局、builder 管线和模块边界。

核心目标：

- 将当前臃肿的 `src/ezdb/ezdb.c` 拆成职责明确的模块。
- 使用 v13 section directory 文件格式，替代不断膨胀的固定 header。
- 保留低内存 spool/stream 架构，继续控制峰值内存。
- 大幅提升入库速度，重点优化 entry postings 构建和 ZIP 解析。
- 保持现有查询能力，包括 archive 搜索、entry 搜索、组合查询、通配符、增量 API。

当前性能基线：

- `build-zip-entries` 已从 minizip 逐 entry 解析升级为 ZIP Central Directory 扫描。
- 大数据集曾实测进入约 `56s` 级别构建路径，其中 ZIP CD scan 约 `6s`，主要瓶颈转移到 postings 构建和写入。
- spool/stream 后峰值内存已显著下降，曾实测约 `573MB` 级别。

## 状态总览

| 模块 | 状态 | 说明 |
| --- | --- | --- |
| ZIP Central Directory scanner | 已完成 | 默认走 CD scan，失败 fallback minizip |
| build-zip-entries spool/stream | 已完成 | 多线程解析落盘 spool，build 阶段流式读取 |
| I/O helper 模块 | 已完成 | 已拆出 `ezdb_io.c/.h` |
| format 基础模块 | 已完成 | 已拆出 `ezdb_format.c/.h`，已加入 v13 header 和 section table helper |
| postings 模块 | 已完成 | builder、tokenizer、container encode、write/read/intersect helper 和 entry index build/write 编排已迁入 `ezdb_postings.c/.h` |
| 删除旧 event spool sorter | 已完成 | 旧 run reader、heap merge、event sorter 写入路径已删除 |
| query 基础模块 | 已完成 | query parser、AST、wildcard、candidate keys、text match helper 已迁入 `ezdb_query.c/.h` |
| v13 section directory 格式 | 部分完成 | build/open 已接入 v13 section table；live entry append 重新 open 已修复并验证 |
| 多线程 postings 构建 | 已完成 | pass 1 count、reduce、slice prepare、pass 2 fill 已并行化 |
| query 主流程拆分 | 部分完成 | parser/helper 已迁出，`search/search-v2/query_entries` 主流程仍在 `ezdb.c` |
| delta 模块拆分 | 未开始 | insert/update/delete/compact 仍在 `ezdb.c` |
| entries 模块拆分 | 部分完成 | entry source、读路径 helper、分页 writer、section build 会话、collect 主循环和 collected section finalize 编排已迁入；外层 build orchestration 仍在 `ezdb.c` |
| build 模块拆分 | 部分完成 | 已新增 `ezdb_build.c/.h`，公开 build API wrapper、build options resolve 和 public stream adapter 已迁入；archive-base core builder 暂留 `ezdb.c` |

当前总体进度估算：约 `87%`。已完成 ZIP CD scan、spool/stream、postings、多线程 postings 和测试体系；entries 写入边界已推进到 collect 主循环、temp/header section 会话和 collected section finalize 编排，entry index build/write 编排已收敛进 postings 模块，build 模块已开始承接公开 API wrapper 和 public stream adapter，剩余主要是 archive-base core builder、query/delta/core 的模块边界继续收敛，以及 `EzdbHeader` 运行期映射依赖收敛。

## 已完成工作

### 1. ZIP Central Directory 直读

状态：已完成。

已实现内容：

- 新增 `tools/zip_cd_scanner.c/.h`。
- 默认通过 ZIP Central Directory 读取 entry 列表。
- 支持普通 ZIP 和 ZIP64 基础路径。
- 读取 filename、compressed size、uncompressed size、mtime、flags。
- 跳过目录 entry。
- CD scan 失败时 fallback 到 minizip。
- 保留 raw path，并继续使用现有 UTF-8/ACP 处理路径。

后续补强：

- 增加 ZIP64 边界 fixture。
- 增加 central directory comment fixture。
- 增加非 UTF-8 raw path 自动化测试。
- 将 fallback 策略正式放入 build options flags。

### 2. spool/stream 构建路径

状态：已完成。

已实现内容：

- `build-zip-entries` 不再把所有 parsed archive/entry 常驻内存。
- 解析线程写入 entry spool shard。
- 每个 archive 记录 chunk 元数据。
- build 阶段按 archive index 顺序读取 chunk，保证 entry id 稳定。
- `EzdbBuildOptions` 已引入 `temp_dir`、`memory_limit_mb`、`flags`、`log_level`、`index_threads`、`zip_threads` 字段。
- 新增 `ezdb_build_snapshot_stream_entries_ex(...)`，旧接口走默认 wrapper。

后续补强：

- 更系统地测试 temp dir 创建、清理和失败路径。
- 将 spool 文件格式文档化到 format/build 文档中。
- 后续 v13 可考虑把 spool reader/writer 拆入 `ezdb_entries.c` 或 `ezdb_build.c`。

### 3. I/O 模块拆分

状态：已完成。

已实现内容：

- 新增 `src/ezdb/ezdb_io.c`。
- 新增 `src/ezdb/ezdb_io.h`。
- 迁出压缩 section 写入/读取 helper：
  - `write_bytes`
  - `maybe_compress_payload`
  - `maybe_compress_section`
  - `write_compressed_section`
  - `read_section_payload`
  - `read_section_into`
  - `write_paged_section`
- 迁出 compressed section varint reader：
  - `SectionVarReader`
  - `section_var_reader_init`
  - `section_var_reader_varuint`
  - `section_var_reader_varuint64`
  - `section_var_reader_close`

后续补强：

- 将 page cache、paged reader、stream paged writer 继续迁入 I/O 或 entries 模块。
- 统一 `_fseeki64` / `ftell` / Windows flush 等平台 I/O 封装。

### 4. format 基础模块与 v13 section directory

状态：部分完成。

已实现内容：

- 新增 `src/ezdb/ezdb_format.c`。
- 新增 `src/ezdb/ezdb_format.h`。
- 将 v13 magic/version、delta type 常量集中到 format 层。
- 将 `EzdbHeader` 移到 format 层。
- 新增 `EZDB_V13_MAGIC`、`EZDB_V13_VERSION`。
- 新增 `EzdbV13Header`。
- 新增 `EzdbSectionDesc`，当前字段包括 `section_id`、`flags`、`offset`、`encoded_size`、`raw_size`、`aux_offset`、`aux_size`、`page_size`、`aux_count`。
- 新增 section table helper：
  - `ezdb_format_v13_header_is_current(...)`
  - `ezdb_format_section_id_is_known(...)`
  - `ezdb_format_validate_section_table(...)`
  - `ezdb_format_find_section(...)`
  - `ezdb_format_write_section_table(...)`
  - `ezdb_format_read_section_table(...)`
- section table 校验已覆盖未知 section id、重复 section id、section/aux 越界、section/aux range 重叠。
- builder 已写出 `EZDB0013` 固定 header 和 section table。
- open 已只接受 v13 magic；旧 v12 文件返回 format error。
- v13 build/open/search 基础 smoke 已通过。
- v13 section table 损坏、重复 section id、offset 越界 smoke 已通过。
- archive 级 delta insert/update/delete 和 compact smoke 已通过。
- live entry append delta 重新 open 已通过，覆盖纯 delta entry 文件和事务替换回放。

未完成内容：

- `EzdbHeader` 仍作为运行期归一化结构存在，open/build 还有较多字段映射依赖。
- v13 delta section table 更新逻辑仍在 `ezdb.c`，尚未拆入 delta/format 更清晰的边界。

### 5. postings 模块拆分

状态：已完成当前计划中的 postings 拆分。

已实现内容：

- 新增 `src/ezdb/ezdb_postings.c`。
- 新增 `src/ezdb/ezdb_postings.h`。
- 迁出 postings 相关常量：
  - gram/token 常量
  - container type 常量
  - density threshold 常量
- 迁出 postings 类型：
  - `PostingBuildEntry`
  - `PostingBuilder`
  - `PostingWriteStats`
  - `GramKeyCallback`
- 迁出 tokenizer/gram/query key：
  - UTF-8 token length
  - gram key 生成
  - text gram key 枚举
  - query key 构建
- 迁出 `PostingBuilder` 管理：
  - init/free/add/find
  - count/fill two-pass
  - adaptive bitset fill
  - remove id
- 迁出 posting container 算法：
  - array size estimate
  - range count/size estimate
  - array/range/bitset encode
- 迁出主 postings 写入路径为 `ezdb_postings_write(...)`：
  - postings 排序
  - container 选择
  - payload 压缩
  - postings section 写入
  - index metadata 生成
- 迁出 entry index build/write 编排：
  - collect 阶段 postings count callback
  - parallel count/reduce
  - slice prepare
  - parallel fill
  - postings payload write
  - entry index metadata write
- 迁出 postings 读取/解码 helper：
  - `ezdb_postings_find_index(...)`
  - `ezdb_postings_load(...)`
  - `ezdb_postings_load_intersected(...)`
  - `ezdb_postings_load_intersected_memory(...)`
- postings builder/query/write API 已统一为 `ezdb_postings_*` 命名，旧宏兼容层已移除。
- `ezdb_postings_build_entry_index(...)` 已接管 entry index 的 count/prepare/fill/write 与性能统计，`ezdb.c` 不再直接持有 entry index 线程代码。
- 删除旧 event spool sorter 死代码：
  - `PostingEventSorter`
  - run reader
  - heap merge
  - event sorter flush/write

后续补强：

- postings 写入已由 postings 模块统一编排，后续可进一步抽象为 section writer 或 build-layer section pipeline。

### 6. query 基础模块拆分

状态：已完成 parser/helper 拆分，主搜索流程仍未迁出。

已实现内容：

- 新增 `src/ezdb/ezdb_query.c`。
- 新增 `src/ezdb/ezdb_query.h`。
- 迁出 query AST/parser：
  - `EzdbQueryNode`
  - `EzdbQueryParser`
  - AND/OR/NOT parse
  - wildcard parse
  - quoted term parse
- 迁出查询匹配 helper：
  - `ezdb_query_parse(...)`
  - `ezdb_query_node_free(...)`
  - `ezdb_query_match_path(...)`
  - `ezdb_query_matches_text(...)`
  - `ezdb_query_contains_ascii_casefold(...)`
  - `ezdb_query_longest_literal_from_wildcard(...)`
  - `ezdb_query_build_candidate_keys(...)`
  - `ezdb_query_is_space(...)`
- `ezdb.c` 中的 `search`、`search-v2`、`query_entries` 已调用 `ezdb_query_*` helper。

后续补强：

- 将 archive/file search 主流程迁出 `ezdb.c`。
- 将 entry search 主流程迁出 `ezdb.c`。
- query 模块后续应只通过 core/entries/postings 内部 API 访问数据。

## 未完成的核心重构

### 1. v13 section directory 文件格式

状态：部分完成。

目标：

- `EZDB_VERSION` 升级到 `13`。
- 不兼容旧库，旧版本直接返回 format error。
- 固定 header 缩小，只保留：
  - magic
  - version
  - flags
  - section_count
  - section_table_offset
  - file/archive/entry 计数
  - metadata/checksum 预留字段
- section table 使用统一结构：

```c
typedef struct EzdbSectionDesc {
    uint32_t section_id;
    uint32_t flags;
    uint64_t offset;
    uint64_t encoded_size;
    uint64_t raw_size;
    uint64_t aux_offset;
    uint64_t aux_size;
    uint32_t page_size;
    uint32_t aux_count;
} EzdbSectionDesc;
```

建议 section id：

| section id | 名称 | 说明 |
| --- | --- | --- |
| 1 | archive records | archive/file base records |
| 2 | dir records | directory tree records |
| 3 | string pool | path/name string pool |
| 4 | archive meta | file ref/usn/drive metadata |
| 5 | entry core | entry id -> archive/path offset/len |
| 6 | entry detail pages | compressed/original size/mtime/raw offset |
| 7 | entry raw pages | entry path/raw path blob |
| 8 | file index | archive/file search index metadata |
| 9 | dir index | directory search index metadata |
| 10 | entry index | entry search index metadata |
| 11 | postings | shared postings payload |
| 12 | delta log | append-only delta frames |
| 13 | metadata | build options/schema/checksum |

实现步骤：

- [x] 新增 v13 header/section descriptor 到 `ezdb_format.h`。
- [x] 编写 section table writer。
- [x] 编写 section table reader。
- [x] 修改 builder：写 payload 后收集 descriptor，最后写 section table 和 header。
- [x] 修改 open：先读固定 header，再加载 section table，再按 section id 初始化 db。
- [x] v12 旧库打开返回 format error。
- [x] 修复 `live-entry-append-batch` 生成库无法重新 open 的问题。
- [ ] 删除对巨大固定 `EzdbHeader` 字段的直接依赖，或将其明确收敛为运行期归一化结构。
- [ ] 将 v13 delta header/section table 更新逻辑从 `ezdb.c` 中进一步分层。

验收：

- [x] v13 新库可构建、打开、搜索。
- [x] v12 旧库打开返回 format error。
- [x] section table 缺失、重复 section id、offset 越界时返回 format error。
- [x] archive delta insert/update/delete/compact smoke 通过。
- [x] live entry append delta 重新 open 通过。

### 2. 多线程 postings 构建

状态：已完成。

目标：

- 消除单线程 postings count/fill 的剩余瓶颈。
- 不再使用旧 event spool/sort 路径。
- 保持 entry id 确定性。
- 峰值内存控制在 `600-800MB`。

设计：

1. ZIP parse 完成后，根据 `ArchiveEntryChunk.entry_count` 计算每个 archive 的 `base_entry_id`。
2. 将 archive index 范围切成 `index_threads` 个连续区间。
3. pass 1：每个线程读取自己的 archive range，对 entry path 枚举 gram，写入线程本地 `key -> count`。
4. reduce：合并所有线程 key，生成全局 key table，按 key 排序。
5. prepare：为每个 key 分配 postings id array，并计算每个线程在该 key 下的 slice offset。
6. pass 2：每个线程重读自己的 archive range，按 `base_entry_id + local_entry_index` 无锁填充自己的 slice。
7. write：按 key 顺序选择 array/range/bitset container，压缩并写 postings section。

确定性规则：

- archive 顺序来自 `zip_files.tsv`。
- entry 顺序来自 zip central directory。
- entry id 永远按 archive 顺序、entry 顺序生成。
- 每个 key 的 postings id 必须升序。

日志：

- `entry_index_threads`
- `entry_index_count_parallel_seconds`
- `entry_index_reduce_seconds`
- `entry_index_prepare_seconds`
- `entry_index_fill_parallel_seconds`
- `entry_index_write_seconds`
- `entry_index_memory_mb`

验收：

- [x] 和当前单线程 postings 输出的 entry 搜索结果一致。
- [x] 连续运行两次 entry id 和搜索结果一致。
- [x] 大数据集目标 postings 构建耗时降至 `15-25s`。

### 3. entries 模块拆分

状态：部分完成。

目标文件：

- `src/ezdb/ezdb_entries.c`
- `src/ezdb/ezdb_entries.h`

计划迁入：

- `EzdbEntrySource`（已迁入 `ezdb_entries.h`）
- `EzdbArrayEntrySource`（已迁入 `ezdb_entries.c/.h`，保留 `open_range/close_range` 并行读取接口）
- `EzdbCompactEntrySource`（已迁入 `ezdb_entries.c/.h`）
- entry stream reader/writer（已迁出 entry paged writer helper）
- entry core writer/reader（已迁出 core 12 字节 encode/decode helper）
- entry detail page writer/reader（已迁出 entry detail reader helper）
- raw blob page writer/reader（已迁出 entry paged writer helper、base raw blob range copy helper 和 delta blob range copy helper）
- entry section writer/build session（已迁出单条 entry core/detail/raw blob 写入、temp file 生命周期、payload copy、page index 写入、header section 字段填充和 collected section finalize 编排；postings/外层 build orchestration 仍在 `ezdb.c`）
- entry collect 主循环（已迁出 source reset/next、archive remap 校验、archive entry count/base collection 和 section 写入；postings count 通过 callback 解耦）
- entry page cache（已迁出 cache entry 类型、加载 helper 和释放 helper）
- entry path/raw path copy helpers（已迁出 entry path copy helper、entry raw path copy helper、base raw blob range copy helper 和 delta blob range copy helper）
- archive -> entry link rebuild

拆分顺序：

1. 迁出 entry disk section encode/decode。（已开始：entry core record encode/decode 已迁入 `ezdb_entries.c/.h`）
2. 迁出 entry detail/raw blob page cache。（已开始：cache entry 类型、加载 helper 和释放 helper 已迁入 `ezdb_entries.c/.h`）
3. 迁出 `entry_path_copy_by_id`、`load_entry_detail`。（已完成二者读取逻辑迁出）
4. 迁出 compact entry source。（已完成）
5. 迁出 array entry source。（已完成）
6. 迁出单条 entry section writer。（已完成：`EzdbEntrySectionWriter` 负责 core/detail/raw blob 写入）
7. 迁出 entry section build session。（已完成：`EzdbEntrySectionBuild` 负责 temp file、finish、payload copy、page index 和 header section 字段）
8. 迁出 entry collect 主循环。（已完成：`EzdbEntryCollectResult` 负责 archive remap/count collection 和 section build；postings count 通过 callback）
9. 迁出 collected section finalize 编排。（已完成：`ezdb_entries_write_collected_sections(...)` 负责 core/detail/raw 写入顺序、entry count/final header 字段和阶段耗时统计）
10. 与 v13 section descriptor 对接。

### 4. query 模块拆分

状态：部分完成。

目标文件：

- `src/ezdb/ezdb_query.c`
- `src/ezdb/ezdb_query.h`

迁移状态：

- 已迁入 query parser/helper：
  - `EzdbQueryNode`
  - `EzdbQueryParser`
  - AND/OR/NOT/wildcard parse
- 已迁入 wildcard/text/candidate key helper。
- 待迁入 archive/file search：
  - `ezdb_search`
  - `ezdb_search_v2`
  - result materialization
- 待迁入 entry search：
  - `query_entries`
  - entry search-v2
  - wildcard post-filter
- postings intersect/load helper 已迁入 `ezdb_postings.c/.h`。

拆分注意：

- query 模块应该只通过 core/entries/postings 的内部 API 访问数据。
- 不直接理解 v13 section table。
- wildcard 逻辑要继续使用 postings tokenizer 的 UTF-8 token 边界规则。

### 5. delta 模块拆分

状态：未开始。

目标文件：

- `src/ezdb/ezdb_delta.c`
- `src/ezdb/ezdb_delta.h`

计划迁入：

- delta disk frame encode/decode。
- insert/update/delete。
- entry append。
- archive upsert。
- write transaction begin/commit/rollback。
- replay delta log。
- compact。

v13 调整：

- delta log 作为独立 section。
- compact 输出 v13 新库。
- delta frame 可增加 checksum 或 length prefix。

### 6. build 模块拆分

状态：部分完成。

目标文件：

- `src/ezdb/ezdb_build.c`
- `src/ezdb/ezdb_build.h`

计划迁入：

- snapshot build 编排。（已开始：公开 `ezdb_build_snapshot*` API wrapper 已迁入 `ezdb_build.c`）
- archive tree 构建。
- directory hash/string pool。
- `ezdb_build_snapshot_*` API 实现。（已迁入 `ezdb_build.c`）
- build options resolve。（已通过 `ezdb_build_resolve_options(...)` 暴露到 build 模块，compact rebuild 已改用该接口）
- build stats/log 输出。
- spool temp dir 清理。
- public `EzdbEntryStream` -> internal `EzdbEntrySource` adapter。（已迁入 `ezdb_build.c`，保留 `open_range/close_range`）

拆分后目标：

- `ezdb.c` 不再包含大型 builder。
- `ezdb.c` 只保留公共 API forwarding 或彻底拆为空壳。

### 7. core 模块拆分

状态：未开始。

目标文件：

- `src/ezdb/ezdb_core.c`
- `src/ezdb/ezdb_core.h`

计划迁入：

- `struct Ezdb` 生命周期。
- open/close。
- stats/info。
- active bitset 基础操作。
- shared utility。
- public error code bridge。

## 建议实施顺序

### 阶段 A：收敛当前 postings 模块

状态：已完成。

- [x] 拆出 postings builder。
- [x] 拆出 tokenizer/gram/query key。
- [x] 拆出 container encode。
- [x] 拆出 `write_postings` 主路径。
- [x] 删除旧 event spool sorter 死代码。
- [x] 迁出 postings 解码/read/intersect helper。
- [x] 将 postings API 改名为 `ezdb_postings_*`，移除宏兼容层。
- [x] 阶段性重构已提交。

### 阶段 B：拆 entries/query

状态：部分完成。

- [x] 新建 `ezdb_entries.c/.h`。
- [ ] 迁出 entry core/detail/raw blob 读写。（已迁出 core encode/decode、detail/raw blob 读路径、分页 writer、array source、entry section writer、section build session 和 collect 主循环；entry postings 编排已迁入 postings，外层构建/final header 编排仍在 `ezdb.c`）
- [x] 新建 `ezdb_query.c/.h`。
- [x] 迁出 query parser。
- [ ] 迁出 archive/entry search。
- [x] 保持 Debug 构建和 smoke test 通过。

### 阶段 C：v13 format

状态：部分完成。

- [x] 定义 v13 fixed header。
- [x] 定义 section descriptor。
- [x] 写 section table writer。
- [x] 写 section table reader。
- [x] 修改 open/build 使用 section id。
- [x] 升级 v13 magic/version 到 `EZDB0013` / `13`。
- [x] v12 文件打开返回 format error。
- [x] 修复 v13 live entry append 重新 open 失败。
- [ ] 删除或收敛旧 `EzdbHeader` 运行期映射依赖。

### 阶段 D：多线程 postings

状态：已完成当前计划。

已完成准备：

- `EzdbEntryStream` 已增加可选 `open_range` / `close_range`，支持为 archive range 创建独立 reader。
- `build-zip-entries` 的 spool stream 已实现独立 range reader，后续线程可安全并发读取不同 archive range。
- entry index 构建日志已输出 `entry_index_threads`、`entry_index_count_parallel_seconds`、`entry_index_reduce_seconds`、`entry_index_fill_parallel_seconds`、`entry_index_write_seconds`。
- pass 1 parallel count 已接入：每个线程使用独立 range reader 生成本地 count builder，再 reduce 合并到全局 builder。
- 在 `test_data\all_zip_files.tsv` 上 6 线程实测：`entry_index_count_seconds 4.785s`、`entry_index_reduce_seconds 0.013s`、`entry_index_fill_seconds 14.265s`、总构建 `26.783s`。
- pass 2 parallel fill 已接入：基于每个线程的本地 count 计算 per-key slice offset，数组/range postings 按 slice 无锁填充，bitset postings 使用原子 OR。
- 在 `test_data\all_zip_files.tsv` 上 6 线程实测：`entry_index_count_seconds 3.337s`、`entry_index_reduce_seconds 0.021s`、`entry_index_prepare_seconds 0.068s`、`entry_index_fill_seconds 5.062s`、总构建 `17.256s`，输出仍为 `97.46 MB`。

- [x] 设计 per-thread key count map。
- [x] 实现 pass 1 parallel count。
- [x] 实现 reduce。
- [x] 实现 slice offset prepare。
- [x] 实现 pass 2 parallel fill。
- [x] 实现 write 阶段复用 postings module。
- [x] 加入性能日志。

### 阶段 E：测试体系

状态：已完成。

- [x] 小 ZIP fixture。
- [x] 空 ZIP。
- [x] ZIP64。
- [x] central directory comment。
- [x] 非 UTF-8 entry name。
- [x] 目录 entry skip。
- [x] search-v2 archive/entry。
- [x] query_entries。
- [x] wildcard。
- [x] insert/update/delete。
- [x] compact。
- [x] 连续运行确定性测试。
- [x] temp dir 清理和失败路径测试。

## 当前代码基线与下次断点

当前代码基线：

- 已提交阶段性重构：`b5a52f6 Refactor ezdb v13 format and query modules`。
- 已修复 v13 live entry append 重新 open：v13 header 严格保留 `base_archive_count/base_entry_count`，并修复 delta replay 中读取 entry path 后未恢复文件位置的问题。
- Debug/Release 构建 `EzdbBench` 通过。
- `src/ezdb/ezdb_entries.c/.h` 已加入 CMake，并迁出 entry core record 12 字节 encode/decode helper、entry detail reader helper、entry paged writer helper、array entry source、`EzdbEntrySectionWriter`、`EzdbEntrySectionBuild`、`EzdbEntryCollectResult` 与 `ezdb_entries_write_collected_sections(...)`；外层 build orchestration 仍待继续迁出。
- `EzdbEntrySource` 内部 stream interface、`EzdbArrayEntrySource` 和 `EzdbCompactEntrySource` 已迁入 `ezdb_entries.c/.h`，array source 保留 `open_range/close_range` 以支持并行 entry index 构建。
- entry detail/raw blob page cache 的 cache entry 类型、加载 helper 和释放 helper 已迁入 `ezdb_entries.c/.h`；entry detail reader helper、entry path copy helper、entry raw path copy helper、base raw blob range copy helper 和 delta blob range copy helper 已迁出，`search-v2` entry emit raw path 读取已统一走 entries helper。
- `src/ezdb/ezdb_query.c/.h` 已加入 CMake。
- `src/ezdb/ezdb_postings.c/.h` 已接管 postings write/read/intersect helper 和 entry index build/write 编排。
- `src/ezdb/ezdb_format.c/.h` 已接管 v13 header 与 section table helper。
- `src/ezdb/ezdb_build.c/.h` 已加入 CMake，并迁入公开 `ezdb_build_snapshot*` API wrapper、build options resolve 边界和 public `EzdbEntryStream` 到 internal `EzdbEntrySource` adapter；`ezdb.c` 暂时保留 archive-base core builder，并通过 `ezdb_build_write_archive_base_core(...)` 供 build/compact 调用。
- 阶段 D 已完成独立 archive range reader 准备：核心 stream API 透传 `open_range/close_range`，zip spool stream 可创建 per-range reader。
- 阶段 D 已完成 pass 1 parallel count、reduce、slice offset prepare 和 pass 2 parallel fill；当前剩余优化点转向 ZIP fixture/测试体系、模块拆分和后续大样本复测。
- 阶段 E 已新增 `EzdbZipFixtureTests`，运行时生成小 ZIP、空 ZIP、带 central directory comment 的 ZIP、含目录 entry 的 ZIP，并验证 `build-zip-entries`、`info`、`search-v2 archive/entry` 回归。
- `EzdbBench` 已新增 `query-entries` 命令，`EzdbZipFixtureTests` 已覆盖 `query_entries`、wildcard、archive insert/update/delete 和 compact 后 entry 查询回归。
- `EzdbZipFixtureTests` 已覆盖连续两次 fresh build 的关键查询确定性、`build-zip-entries` temp dir 成功清理和缺失 ZIP 解析失败路径；同时修复 compact 使用同一 archive id map 导致 entry archive remap 被覆盖的问题。
- `EzdbZipFixtureTests` 已加入手写最小 ZIP64 central directory fixture 和非 UTF-8 raw entry name fixture，覆盖 ZIP64 extra 解析、ACP raw path 解码和 raw path 保留。
- 2026-06-06 本机继续阶段 B：迁出 `EzdbArrayEntrySource` 到 entries 模块；`cmake --build cmake-build-codex-release --config Release --target EzdbBench` 通过；`ctest --test-dir cmake-build-codex-release -C Release -R EzdbZipFixtureTests --output-on-failure` 通过。
- 2026-06-06 本机继续阶段 B：新增 `EzdbEntrySectionWriter`，迁出单条 entry core/detail/raw blob 写入逻辑；Release `EzdbBench` 构建通过，`EzdbZipFixtureTests` 通过。
- 2026-06-06 本机 `test_data\all_zip_files.tsv` 6 线程复测：`5559` 个 ZIP、`1171025` 个 entry；`zip_parse_seconds 1.473s`，`entry_total_seconds 14.628s`，`zip_total_parse_to_build_seconds 16.250s`，峰值工作集 `292.83MB`，输出 `97.46MB`。
- 2026-06-06 本机继续阶段 B：新增 `EzdbEntrySectionBuild`，迁出 entry section temp file 生命周期、finish、payload copy、page index 写入和 header section 字段填充；同时删除 `ezdb.c` 中对应死 helper；Release `EzdbBench` 构建通过，`EzdbZipFixtureTests` 通过。
- 2026-06-06 本机 `test_data\all_zip_files.tsv` 6 线程复测：`5559` 个 ZIP、`1171025` 个 entry；`zip_parse_seconds 1.414s`，`entry_total_seconds 16.417s`，`zip_total_parse_to_build_seconds 17.961s`，峰值工作集 `292.78MB`，输出 `97.46MB`。
- 2026-06-06 本机继续阶段 B：新增 `EzdbEntryCollectResult` / `ezdb_entries_collect_sections`，迁出 entry source collect 主循环、archive remap 校验、archive entry count/base collection 和 section 写入；postings count 通过 callback 保持在 postings 模块；同时补齐 collect 失败路径的 temp 文件清理。Release `EzdbBench` 构建通过，`EzdbZipFixtureTests` 通过。
- 2026-06-06 本机 `test_data\all_zip_files.tsv` 6 线程复测：`5559` 个 ZIP、`1171025` 个 entry；`zip_parse_seconds 1.392s`，`entry_total_seconds 15.468s`，`zip_total_parse_to_build_seconds 16.977s`，峰值工作集 `292.86MB`，输出 `97.46MB`。
- 2026-06-06 本机继续阶段 B/A：新增 `ezdb_postings_build_entry_index(...)` 和 `EzdbEntryIndexBuildStats`，迁出 entry index parallel count/reduce、slice prepare、parallel fill、postings write 和 entry index metadata write；`ezdb.c` 删除对应 Windows thread 静态实现。Release `EzdbBench` 构建通过，`EzdbZipFixtureTests` 通过。
- 2026-06-06 本机 `test_data\all_zip_files.tsv` 6 线程复测：`5559` 个 ZIP、`1171025` 个 entry；`zip_parse_seconds 1.388s`，`entry_total_seconds 14.658s`，`zip_total_parse_to_build_seconds 16.161s`，峰值工作集 `293.00MB`，输出 `97.46MB`。
- 2026-06-06 本机继续阶段 B/F：新增 `EzdbEntryFinalizeStats` / `ezdb_entries_write_collected_sections(...)`，迁出 collected entry section 的 core/detail/raw 写入顺序、entry count/final header 字段和阶段耗时统计；`ezdb_write_entries_from_source` 进一步收缩为 collect + postings + entries finalize 编排。Release `EzdbBench` 构建通过，`EzdbZipFixtureTests` 通过。
- 2026-06-06 本机 `test_data\all_zip_files.tsv` 6 线程复测：`5559` 个 ZIP、`1171025` 个 entry；`zip_parse_seconds 1.580s`，`entry_total_seconds 12.666s`，`zip_total_parse_to_build_seconds 14.375s`，峰值工作集 `292.78MB`，输出 `97.46MB`。
- 2026-06-06 本机继续阶段 F：新增 `ezdb_build.c/.h` 并接入 CMake；迁出公开 build snapshot API wrapper、public stream adapter 和 build options resolve 边界；compact rebuild 改走 `ezdb_build_resolve_options(...)` / `ezdb_build_write_archive_base_core(...)`。Release `EzdbBench` 构建通过，`EzdbZipFixtureTests` 通过。
- 2026-06-06 本机 `test_data\all_zip_files.tsv` 6 线程复测：`5559` 个 ZIP、`1171025` 个 entry；`zip_parse_seconds 1.621s`，`entry_total_seconds 14.446s`，`zip_total_parse_to_build_seconds 16.250s`，峰值工作集 `292.86MB`，输出 `97.46MB`。

当前工作区中仍有非本计划代码提交项：

- `external/zlib` 子模块存在独立 dirty 状态。
- 本文档 `doc/ezdb-v13-refactor-plan.md` 当前用于计划追踪。

下次继续的首要断点：

- 下一步继续阶段 F/C 的 build/v13 header 收敛，优先把 archive tree/string pool/base section 写入和 file/dir postings 编排继续迁入 `ezdb_build.c`，或先切走 `EzdbHeader` v13 运行期映射依赖。

## 风险与注意事项

- v13 不兼容旧库，需要明确错误提示和版本检查。
- 多线程 postings 要严格保证 postings id 升序，否则查询交集和结果顺序可能出错。
- section directory 改造应先保持 reader 语义不变，再优化布局。
- query 拆分时避免让 query 直接依赖 build 内部结构。
- entries 拆分时要特别小心 delta entry 与 base entry 的读取路径。
- 当前 `external/zlib` 子模块存在独立 dirty 状态，不属于本计划改动。
- 当前未跟踪 `prompt.md` 不属于本计划改动。
