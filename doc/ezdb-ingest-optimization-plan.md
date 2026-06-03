# ezdb 入库性能优化设计与开发计划

## 1. 背景

在完全相同环境下，初始归档解析入库日志显示：

| 后端 | parse_threads | parse_total | entries | parse_elapsed_sec | archives_per_sec | entries_per_sec |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| SQLite | 6 | 3413 | 547443 | 19.537 | 174.70 | 28020.98 |
| ezdb | 6 | 3413 | 547443 | 57.364 | 59.50 | 9543.32 |

当前 ezdb 初始解析入库比 SQLite 慢约 2.94 倍。目标是最大程度提升 ezdb 入库速度，在可接受深度重构、可升级格式版本、不兼容旧 `.ezdb` 的前提下，把初始入库路径改造成适合百万到千万级 entries 的批量导入管线，同时不明显牺牲查询、打开、compact 和增量监控性能。

## 2. 性能瓶颈判断

### 2.1 写入端串行化

`Indexer` 当前使用多个解析 worker 并行读取归档，但所有解析结果都进入一个消费循环，由该循环串行调用 `IndexStore` 写入。`parse_elapsed_sec` 包含解析、排队、写库和写库反压时间。只要 ezdb 写入端慢，6 个解析线程会被单写入消费者拖住。

### 2.2 archive entry replacement 删除阶段近似 O(N²)

`ezdb_begin_replace_archive_entries` 会写入 archive entries delete delta，并在内存中遍历当前 `entry_count`，检查每个 entry 是否属于目标 archive。初始导入期间，archive 按顺序入库，`entry_count` 不断增长，因此第 N 个 archive 的 begin replacement 会扫描前面所有已入库 entries。

在 3413 个 archive、547443 个 entries、平均约 160 entries/archive 的样本中，累计检查量可粗略达到数亿次，属于首要结构性瓶颈。

### 2.3 header flush 与 `_commit()` 过于频繁

ezdb 的 `write_header()` 会写文件头并执行 `fflush()` + `_commit()`。当前每个 archive entry replacement 常见路径包括：

1. begin replace：写 delete frame 后 flush header。
2. append entries：写 batch 后 flush header。
3. finish replace：再次 flush header。

对 3413 个 archive，可能产生近万次强制持久化点。SQLite 则主要依赖事务批量提交。

### 2.4 ezdb 在入库时同步维护 delta entry 搜索索引

ezdb append entry 时同步维护 delta entry gram index；SQLite 当前 entries 表只维护 `archive_id` 索引，没有给 entry path 做等价 gram/FTS 索引。当前对比中 ezdb 做了更多搜索准备工作，因此初始入库阶段负担更重。

### 2.5 小写入和 per-entry 元数据维护成本

每条 entry delta 逐条写 header/path/raw bytes，并逐条更新 arrays、bitset、delta refs 和 gram index。对几十万 entries，这会产生大量小 `fwrite`、函数调用和分配/转换开销。

## 3. 总体目标

### 3.1 性能目标

以当前样本为第一阶段目标：

1. 第一阶段：ezdb parse 入库从 57s 降到 25s 内。
2. 第二阶段：接近或超过 SQLite 的 19.5s。
3. 第三阶段：在延迟建索引 + 批量 compact 方案下，让“解析入库阶段”显著快于 SQLite，同时单独统计 compact/index build 时间，避免只是转移成本。

### 3.2 正确性目标

1. archive 与 entries 关系正确。
2. replace archive entries 后旧 entries 不再参与搜索、查询和 row cache 回读。
3. 删除 archive 时关联 entries 级联失效。
4. compact 后 active archive/entry 数量、查询结果、搜索结果一致。
5. 事务 rollback 不污染主库状态。
6. 初始导入、SQLite import、USN 增量监控都能走同一套可靠语义。

### 3.3 格式兼容目标

如果新增磁盘 section、header 字段、entry generation 语义或 bulk import 标志导致旧格式无法兼容，则将 magic 从 `EZDB0011` 升级到 `EZDB0012`，并声明 v12 不兼容 v11，需要重建 `.ezdb`。

## 4. 方案总览

建议按以下优先级实施：

1. 增加性能计数器与 benchmark，先建立可重复观测指标。
2. 为 entries 建立 archive 到 entry ids 的邻接索引，消除 delete replacement 全量扫描。
3. 改造 entry replacement 事务语义，降低 header flush 和 `_commit()` 次数。
4. 实现 entry batch 连续缓冲写入，减少小写入。
5. 为初始 bulk import 增加延迟构建 delta entry gram index 模式。
6. 将新能力接入 `Indexer` 初始解析管线，并按需升级 ezdb v12 格式。

## 5. 详细设计

### 5.1 性能计数器与 benchmark

#### 5.1.1 新增 `EzdbPerfStats`

在 C API 中新增性能统计结构，建议字段：

```c
typedef struct EzdbPerfStats {
    uint64_t entry_append_count;
    uint64_t entry_append_bytes;
    uint64_t entry_append_batch_count;
    uint64_t entry_delete_archive_count;
    uint64_t entry_delete_scan_count;
    uint64_t entry_delete_touched_count;
    uint64_t header_write_count;
    uint64_t file_commit_count;
    uint64_t write_txn_begin_count;
    uint64_t write_txn_commit_count;
    uint64_t delta_entry_index_add_count;
    uint64_t delta_entry_index_remove_count;
    double entry_delete_elapsed_sec;
    double entry_append_elapsed_sec;
    double header_write_elapsed_sec;
    double file_commit_elapsed_sec;
    double delta_entry_index_add_elapsed_sec;
    double compact_elapsed_sec;
} EzdbPerfStats;
```

新增 API：

```c
int ezdb_get_perf_stats(Ezdb* db, EzdbPerfStats* out_stats);
int ezdb_reset_perf_stats(Ezdb* db);
```

#### 5.1.2 Indexer 日志

初始解析完成日志追加 ezdb 专用统计：

- `ezdb_entry_append_count`
- `ezdb_entry_append_batch_count`
- `ezdb_entry_delete_scan_count`
- `ezdb_header_write_count`
- `ezdb_file_commit_count`
- `ezdb_delta_index_add_count`
- `ezdb_writer_elapsed_sec`

#### 5.1.3 Bench 命令

扩展 `tools/ezdb_bench.c`：

- `bulk-entry-import <archives.tsv> <entries.tsv> <output.ezdb>`
- `replace-archive-entries <db.ezdb> <archive_id> <entry_count> [batch_size]`
- `compact-after-import <db.ezdb>`
- `perf-info <db.ezdb>`

用于分别压测：只 append、replace、compact、搜索和打开耗时。

### 5.2 archive-entry 邻接索引

#### 5.2.1 目标

把 `replace archive entries` 删除阶段从“扫描所有 entries”改为“只遍历该 archive 的 entries”。

#### 5.2.2 内存结构

推荐使用链表式邻接表，append 成本低，适合 delta entry 持续追加：

```c
uint32_t* archive_entry_heads;  // length = file_count, UINT32_MAX means empty
uint32_t* archive_entry_tails;  // length = file_count
uint32_t* entry_archive_next;   // length = entry_count, next entry id in same archive
```

追加 entry 成功后：

1. `entry_archive_next[id] = UINT32_MAX`。
2. 如果 `archive_entry_heads[archive_id] == UINT32_MAX`，head/tail 均设为 id。
3. 否则 `entry_archive_next[tail] = id`，再更新 tail。

删除某 archive entries 时：

1. 从 `archive_entry_heads[archive_id]` 开始沿 `entry_archive_next` 遍历。
2. 对 active entry 清 active bit。
3. 如果 entry 属于 delta index，则移除 delta index path。
4. 不扫描其他 archive 的 entries。

#### 5.2.3 open/replay/compact 后重建

打开数据库时，在 base entries 和 delta entries 加载/replay 完成后，统一调用：

```c
static int rebuild_archive_entry_adjacency(Ezdb* db);
```

该函数线性扫描 `entry_count` 一次，根据 `entry_archive_ids[e]` 重建 head/tail/next。这个 O(N) 成本只发生在 open 或 compact 后，不发生在每个 archive replacement。

#### 5.2.4 删除语义

replace delete frame 只清 active bit，不立即断开邻接链，避免链表中删除节点的复杂维护。邻接链允许包含 inactive entries；后续遍历时跳过 inactive entries。compact 会重建干净链表。

#### 5.2.5 generation 可选增强

如果需要进一步减少 replace delete 对旧 entries 的逐个 bitset 操作，可引入 archive entry generation：

```c
uint32_t* archive_entry_generation;
uint32_t* entry_generation;
```

replace begin 时只递增 archive generation。查询和搜索过滤 `entry_generation[e] == archive_entry_generation[archive_id]`。这种方案会进一步减少删除成本，但会影响搜索过滤路径和磁盘语义，建议作为第二阶段增强。

### 5.3 entry replacement 事务化

#### 5.3.1 核心原则

单条写入仍可保持“调用成功前持久化”，但初始解析 bulk import 必须允许延迟 header flush，在 commit 时统一落盘。

#### 5.3.2 修改点

1. `ezdb_begin_replace_archive_entries`：
   - 如果 `db->write_txn_active` 为 false，保持现有强持久化语义。
   - 如果为 true，`append_entry_delete_archive_frame(..., flush_now=0)`。

2. `ezdb_append_archive_entries`：
   - 如果 `db->write_txn_active` 为 true，append batch 后不 `write_header()`。
   - 如果为 false，保持现有调用完成后 durable。

3. `ezdb_finish_replace_archive_entries`：
   - 如果 `db->write_txn_active` 为 true，只完成内存状态，不 `write_header()`。
   - 如果为 false，保持现有 `write_header()`。

4. `ezdb_commit_write`：
   - 写 commit frame。
   - 写 header。
   - 单次 `_commit()`。

#### 5.3.3 初始解析分段事务

`Indexer` 初始解析不建议持有全程单个大事务。推荐分段提交策略：

- 每 128 个 archive commit 一次；或
- 每 65536 entries commit 一次；或
- 每 2 秒 commit 一次。

三者组合，满足任一条件即 commit。这样可在性能、崩溃恢复和 UI 可见性之间平衡。

### 5.4 batch 连续缓冲写入

#### 5.4.1 目标

把 `ezdb_append_archive_entries` 中每条 entry 的多次小 `fwrite` 合并为 batch 级连续写入。

#### 5.4.2 流程

1. 第一遍遍历 `EzdbEntryRecord[]`：校验参数，计算每条 entry 的 path/raw 长度和 batch 总字节数。
2. 分配 batch buffer。
3. 第二遍编码：
   - `EzdbEntryDeltaDiskHeader`
   - `entry_path`
   - `entry_raw_path`
4. 一次 `fwrite(buffer, 1, batch_bytes, fp)`。
5. 写入成功后批量更新内存结构：
   - `entry_archive_ids`
   - `entry_path_offsets`
   - `entry_path_lens`
   - `delta_entry_refs`
   - `active_entry_bits`
   - `delta_entry_bits`
   - `archive_entry_heads/tails/next`
   - 可选 delta gram index。
6. 失败时恢复 header counters、delta offset/size、reserved offset，并清理已扩容但未提交的状态。

#### 5.4.3 内存上限

避免极大 batch 占用过多内存。建议：

- 单次 batch buffer 目标 4 MiB 到 16 MiB。
- 如果 entries 太多或路径太长，自动拆成多个内部 chunk。

### 5.5 bulk import 与延迟建索引

#### 5.5.1 API 设计

新增 bulk import flags：

```c
#define EZDB_WRITE_BULK_ENTRY_IMPORT      0x00000001u
#define EZDB_WRITE_DEFER_ENTRY_GRAM_INDEX 0x00000002u
```

`ezdb_begin_write(Ezdb* db, uint32_t flags)` 接收 flags，并记录在 `db->write_txn_flags`。

#### 5.5.2 延迟索引模式

当处于 `EZDB_WRITE_DEFER_ENTRY_GRAM_INDEX` 时：

1. `append_entry_delta_disk` / batch append 不调用 `delta_entry_index_add_path`。
2. entry 仍写入 delta log，active bit 和 archive adjacency 仍更新。
3. 查询语义有两种可选策略：
   - 初始解析阶段 UI 不查询未 compact 的 delta entries。
   - 对未索引 delta entries 使用 fallback scan，结果正确但慢。
4. 初始解析结束后执行 compact，一次性构建 base entry postings。

#### 5.5.3 增量监控保持实时索引

USN 监控产生的小批量 replace 不使用 defer index，仍保持实时 delta gram index，避免监控阶段搜索不到新增 entries。

### 5.6 Indexer bulk import 管线

#### 5.6.1 新增能力探测

在 `IndexStore` 抽象中新增可选 bulk API：

```cpp
virtual bool SupportsBulkEntryImport() const;
virtual bool BeginBulkEntryImport(std::wstring* err);
virtual bool CommitBulkEntryImport(std::wstring* err);
virtual bool RollbackBulkEntryImport();
virtual bool MaybeCommitBulkEntryImport(size_t archivesDone, size_t entriesDone, std::wstring* err);
```

SQLite 后端可以返回 false 或维持原逻辑。Ezdb 后端启用分段事务与延迟索引。

#### 5.6.2 初始解析消费循环

在初始解析阶段：

1. 打开 bulk import。
2. 消费 begin/append/finish 批。
3. 达到分段提交条件时 commit 并立刻 begin 新事务。
4. 解析结束 commit。
5. 执行 compact，把 delta entries 吸收到 base。

#### 5.6.3 日志字段

新增：

- `writer_elapsed_sec`
- `writer_wait_elapsed_sec`
- `parser_wait_elapsed_sec`
- `ready_queue_peak`
- `bulk_commit_count`
- `entries_per_commit_avg`
- `archives_per_commit_avg`

用于判断瓶颈是否从写入转回解析。

## 6. 格式版本升级计划

### 6.1 是否必须升级

如果只新增内存邻接索引、事务 flush 策略和 batch fwrite，不需要升级磁盘格式。

如果采用以下任一改动，则升级到 `EZDB0012`：

1. header 新增持久字段。
2. 新增 archive-entry adjacency section。
3. 新增 entry generation 持久语义。
4. 新增未索引 delta entry 标志，需要 open/replay 区分。
5. 修改 delta frame 编码格式。

### 6.2 v12 文档要求

升级时同步更新：

- `src/ezdb/ezdb.c` 中 magic/version。
- `src/ezdb/ezdb.h` 中公开结构和 API。
- `doc/ezdb-development-plan.md` 当前格式版本说明。
- `tools/ezdb_bench.c` info 输出。
- SQLite import 和 compact 构建路径。

### 6.3 v12 兼容策略

不兼容 v11。启动时发现旧 `.ezdb`：

1. 如果存在 SQLite 源库，可重新导入。
2. 如果没有 SQLite 源库，删除/备份旧 `.ezdb` 后重新全量扫描。
3. 日志明确输出旧版本与新版本。

## 7. 测试计划

### 7.1 单元/功能测试

1. 空库 open/close。
2. 单 archive replace entries。
3. 多 archive replace entries。
4. replace 同一 archive 两次，确认旧 entries 不可见。
5. 删除 archive，确认 entries 级联不可见。
6. compact 前后查询结果一致。
7. rollback 后 entries、delta size、active count 恢复。
8. open/replay 后 archive adjacency 正确。
9. 延迟索引模式下 compact 后搜索结果正确。

### 7.2 性能测试

1. 当前样本：3413 archives / 547443 entries。
2. 小 archive 多：10000 archives / 500000 entries。
3. 大 archive 少：100 archives / 1000000 entries。
4. 混合路径长度，包含长 UTF-8 路径和 raw path。
5. 监控增量：每批 1/10/100 archives replace。

### 7.3 回归指标

每次优化记录：

- parse_elapsed_sec
- compact_elapsed_sec
- total_initial_index_elapsed_sec
- entries_per_sec
- header_write_count
- file_commit_count
- entry_delete_scan_count
- open_elapsed_sec
- search p50/p95/p99
- `.ezdb` file size

## 8. 迭代里程碑

### Milestone 1：可观测性

- 增加 `EzdbPerfStats`。
- 增加 bench 命令。
- 初始解析日志输出写入分解指标。

验收：能解释 57s 中 delete scan、flush、index build、fwrite 各占多少。

### Milestone 2：消除 O(N²) 删除扫描

- 实现 archive-entry 邻接索引。
- replace delete 只遍历目标 archive entries。

验收：`entry_delete_scan_count` 从数亿级降到接近实际被删除 entries 数。

### Milestone 3：事务化 entry replacement

- entry replacement 在 write transaction 中不频繁 header flush。
- Indexer 初始解析启用分段事务。

验收：`file_commit_count` 从 archive 级别下降到批次级别。

### Milestone 4：batch buffer 写入

- `ezdb_append_archive_entries` 使用连续 buffer/chunk 写入。

验收：append 阶段 CPU 与 IO 时间下降，entries/s 提升。

### Milestone 5：延迟 entry gram index

- bulk import 可跳过实时 delta gram index。
- compact 一次性构建 base entry index。

验收：parse 入库阶段接近或超过 SQLite；compact 后搜索性能不下降。

### Milestone 6：格式升级与完整集成

- 如需要，升级到 `EZDB0012`。
- 更新文档、工具和导入路径。
- 全量回归。

验收：新库构建、打开、搜索、compact、监控均通过测试。

## 9. 风险与权衡

| 风险 | 影响 | 缓解 |
| --- | --- | --- |
| 延迟建索引导致解析中搜索结果不完整 | UI 体验风险 | 初始扫描阶段标记“索引构建中”，或 fallback scan 未索引 delta entries |
| 分段事务导致崩溃后部分 archive 已提交 | 可接受但需 replay 正确 | batch begin/commit frame 明确事务边界 |
| 邻接链包含 inactive entries | delete 遍历仍可能变长 | compact 清理；必要时引入 generation |
| batch buffer 占用内存 | 大 archive 风险 | 4-16 MiB chunk 上限 |
| v12 不兼容 v11 | 用户需重建 | 启动检测旧版本并自动重建/重新扫描 |
| 过度优化入库影响搜索 | 核心体验风险 | compact 后搜索 p95/p99 必须纳入验收 |

## 10. 推荐先做的最小闭环

第一轮开发不必一次完成全部深度重构，建议最小闭环为：

1. 增加 `EzdbPerfStats`。
2. 实现内存 archive-entry 邻接索引。
3. entry replacement 在 write transaction 中跳过 begin/append/finish 的即时 `write_header()`。
4. Indexer 初始解析每 128 archive 或 65536 entries 分段 commit。
5. 跑当前样本，比较 SQLite、旧 ezdb、新 ezdb。

如果第一轮后 ezdb 已从 57s 降到 25s 内，再继续 batch buffer 与延迟建索引；如果仍慢，则用 perf stats 精确定位剩余瓶颈。
