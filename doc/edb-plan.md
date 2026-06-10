# 重新开发专用数据库 ezdb2，新名称：edb

## 1. 概述

edb 是为 EveryZip 设计的专用嵌入式数据库，用于存储磁盘上压缩包文件（.zip/.7z/.rar）及其内部条目的元数据，并提供基于路径的高性能全文搜索。

核心能力：
- 存储压缩包记录（EdbArchiveRecord）和条目记录（EdbEntryRecord），两者有一对多关系
- 增删改查（CRUD）
- 删除压缩包时级联删除其所有条目
- 支持只存 EdbFileRecord（无条目），可用于索引磁盘上所有文件
- 基于路径的全文搜索，支持中文/英文混合搜索

设计原则（优先级从高到低）：
1. **代码简洁** — 易于理解和维护，人类可审查
2. **查询性能** — 毫秒级搜索响应
3. **存储效率** — 紧凑的二进制格式，合理的内存占用

## 2. 代码规范

- 纯 C99 实现
- 源码放在 `src/edb/` 目录下，以 `edb_*.h/c` 风格命名
- 公开类型和函数以 `edb_` 开头，内部函数以 `edb_internal_` 或带模块前缀
- 公开 API 仅通过 `edb.h` 暴露，不泄露内部类型
- 使用 4 空格缩进
- 错误码返回 int，所有公开 API（除 void 函数外）都返回错误码
- 跨平台：不使用 Windows 专属 API，需要平台相关功能时用条件编译

## 3. 数据模型

### 3.1 记录类型

```c
/* 磁盘上的文件记录（包含 NTFS 文件系统级别的标识信息） */
typedef struct EdbFileRecord {
    char         drive_letter;      /* 驱动器盘符（如 'C'） */
    uint64_t     file_ref_number;   /* NTFS 文件引用编号，唯一标识文件 */
    int64_t      usn;               /* NTFS USN 日志序号 */
    const char*  file_path;         /* 文件完整路径（UTF-8） */
    uint64_t     file_size;         /* 文件大小（字节） */
    uint64_t     modified_time;     /* 最后修改时间（Windows FILETIME 格式） */
} EdbFileRecord;

/* 压缩包文件（.zip / .7z / .rar） */
typedef EdbFileRecord EdbArchiveRecord;

/* 压缩包内的条目（文件或目录） */
typedef struct EdbEntryRecord {
    uint32_t     archive_id;        /* 所属压缩包的 ID（edb_upsert_archive 返回） */
    const char*  entry_path;        /* 条目路径（UTF-8 字符串） */
    const void*  entry_raw_path;    /* 原始路径（压缩包内部编码，如 UTF-16） */
    uint32_t     entry_raw_path_len;/* 原始路径字节长度 */
    int64_t      compressed_size;   /* 压缩后大小（-1 表示无压缩信息） */
    uint64_t     original_size;     /* 原始大小 */
    uint64_t     modified_time;     /* 修改时间（Windows FILETIME 格式） */
} EdbEntryRecord;
```

### 3.2 关键约定

- **archive_id**：数据库内部的自增行 ID（1-based）。调用者通过 `edb_upsert_archive` 的 `out_id` 获取。在 `EdbEntryRecord.archive_id` 中使用此值关联到父压缩包。在快照构建时，archive_id 就是 archive 在数组中的索引（从 1 开始）。
- **级联删除**：删除一个 archive 时，其所有关联 entry 自动标记为非活跃。
- **内存所有权**：`EdbFileRecord` 和 `EdbEntryRecord` 中的 `const char*` / `const void*` 指针由调用者持有，edb 内部会拷贝数据，调用者在 API 返回后即可释放。
- **modified_time**：使用 Windows FILETIME 格式（100 纳秒间隔，自 1601-01-01 UTC）。
- **file-only 模式**：只传入 `EdbFileRecord`，不传入 `EdbEntryRecord`（entry_count=0），可用于索引磁盘全部文件。

## 4. 公开 API

### 4.1 类型定义

```c
typedef struct Edb Edb;  /* 不透明数据库句柄 */
```

#### 搜索结果类型

```c
/* 搜索结果类型：区分压缩包匹配和条目匹配 */
typedef enum EdbSearchKind {
    EDB_RESULT_ARCHIVE = 1,  /* 匹配的是压缩包路径 */
    EDB_RESULT_ENTRY    = 2  /* 匹配的是条目路径 */
} EdbSearchKind;

/* 压缩包查询结果（调用方需 edb_free_archive_result 释放） */
typedef struct EdbArchiveResult {
    uint32_t id;               /* 压缩包 ID */
    char     drive_letter;     /* 驱动器盘符 */
    uint64_t file_ref_number;  /* NTFS 文件引用编号 */
    int64_t  usn;              /* NTFS USN 日志序号 */
    char*    file_path;        /* 压缩包完整路径（需 free） */
    uint64_t file_size;        /* 文件大小 */
    uint64_t modified_time;    /* 修改时间 */
} EdbArchiveResult;

/* 条目查询结果（调用方需 edb_free_entry_result 释放） */
typedef struct EdbEntryResult {
    uint32_t id;               /* 条目 ID */
    uint32_t archive_id;       /* 所属压缩包 ID */
    char*    archive_path;     /* 所属压缩包路径（需 free） */
    char*    entry_path;       /* 条目路径（需 free） */
    void*    entry_raw_path;   /* 原始路径数据（需 free） */
    uint32_t entry_raw_path_len;/* 原始路径字节长度 */
    int64_t  compressed_size;  /* 压缩后大小 */
    uint64_t original_size;    /* 原始大小 */
    uint64_t modified_time;    /* 修改时间 */
} EdbEntryResult;

/* 统一搜索结果（调用方需 edb_free_search_result 释放） */
typedef struct EdbSearchResult {
    EdbSearchKind kind;        /* 结果类型 */
    uint32_t id;               /* 记录 ID */
    uint32_t archive_id;       /* 所属压缩包 ID（仅条目类型有效） */
    char     drive_letter;     /* 驱动器盘符 */
    uint64_t file_ref_number;  /* NTFS 文件引用编号 */
    int64_t  usn;              /* NTFS USN 日志序号 */
    char*    archive_path;     /* 压缩包路径（需 free） */
    char*    entry_path;       /* 条目路径（需 free） */
    void*    entry_raw_path;   /* 原始路径数据（需 free） */
    uint32_t entry_raw_path_len;/* 原始路径字节长度 */
    int64_t  compressed_size;  /* 压缩后大小 */
    uint64_t original_size;    /* 原始大小 */
    uint64_t file_size;        /* 文件大小（压缩包级别） */
    uint64_t modified_time;    /* 修改时间 */
} EdbSearchResult;
```

#### 搜索范围标志位

```c
#define EDB_SEARCH_ARCHIVE_PATH  0x01u  /* 搜索压缩包路径 */
#define EDB_SEARCH_ENTRY_PATH    0x02u  /* 搜索条目路径 */
#define EDB_SEARCH_COMBINED_PATH 0x04u  /* 搜索组合路径（压缩包路径 + 条目路径） */
#define EDB_SEARCH_ALL (EDB_SEARCH_ARCHIVE_PATH | EDB_SEARCH_ENTRY_PATH)
```

#### 分页查询

```c
/* 条目分页查询参数 */
typedef struct EdbEntryQuery {
    const char* keyword;       /* 搜索关键词（支持通配符、AND/OR/NOT 语法） */
    uint32_t    scope;         /* 搜索范围（EDB_SEARCH_* 标志位） */
    int         sort_column;   /* 排序列：0=默认(按ID), 1=路径, 2=大小, 3=时间 */
    int         sort_ascending;/* 1=升序, 0=降序 */
    uint32_t    offset;        /* 分页偏移量 */
    uint32_t    limit;         /* 每页返回数量 */
} EdbEntryQuery;

/* 条目分页查询结果（调用方需 edb_free_query_page 释放） */
typedef struct EdbEntryQueryPage {
    uint64_t   total_count;    /* 满足条件的总条目数 */
    uint32_t   returned_count; /* 本页返回的条目数量 */
    uint32_t*  ids;            /* 本页条目 ID 数组（需 free） */
} EdbEntryQueryPage;
```

#### 统计信息

```c
/* 数据库各部分统计信息 */
typedef struct EdbStats {
    uint32_t archive_count;        /* 压缩包总记录数（含非活跃） */
    uint32_t active_archive_count; /* 活跃压缩包记录数 */
    uint32_t entry_count;          /* 条目总记录数（含非活跃） */
    uint32_t active_entry_count;   /* 活跃条目记录数 */
    uint64_t file_size;            /* 数据库文件总大小 */
    uint64_t archive_records_size; /* 压缩包记录段大小 */
    uint64_t archive_strings_size; /* 压缩包字符串池大小 */
    uint64_t entry_core_size;      /* 条目核心记录段大小 */
    uint64_t entry_detail_size;    /* 条目详情段大小 */
    uint64_t entry_path_size;      /* 条目路径段大小 */
    uint64_t raw_blob_size;        /* 原始路径数据段大小 */
    uint64_t archive_index_size;   /* 压缩包索引段大小 */
    uint64_t archive_postings_size;/* 压缩包倒排列表段大小 */
    uint64_t entry_index_size;     /* 条目索引段大小 */
    uint64_t entry_postings_size;  /* 条目倒排列表段大小 */
} EdbStats;
```

#### 流式条目接口

```c
/* 流式迭代器接口，用于大规模条目数据的逐条读取 */
typedef struct EdbEntryStream {
    void* user_data;
    int  (*reset)(void* user_data);
    int  (*reset_range)(void* user_data, uint32_t archive_begin, uint32_t archive_end);
    int  (*next)(void* user_data, EdbEntryRecord* out_record);
    int  (*open_range)(void* user_data, uint32_t archive_begin, uint32_t archive_end,
                       struct EdbEntryStream* out_stream);
    void (*close_range)(struct EdbEntryStream* stream);
} EdbEntryStream;
```

#### 构建选项

```c
#define EDB_BUILD_ENTRY_INDEX         0x01u  /* 为条目路径构建倒排索引 */
#define EDB_BUILD_POSTING_COMPRESSION 0x02u  /* 对倒排列表启用 zlib 压缩 */
#define EDB_BUILD_DEFAULT_FLAGS (EDB_BUILD_ENTRY_INDEX | EDB_BUILD_POSTING_COMPRESSION)

typedef struct EdbBuildOptions {
    const char* temp_dir;       /* 临时文件目录（NULL = 输出路径同目录） */
    uint32_t    memory_limit_mb;/* 内存限制 MB（0 = 默认 512） */
    uint32_t    flags;          /* 构建标志位（EDB_BUILD_* 组合） */
    uint32_t    log_level;      /* 日志级别（0=静默） */
    uint32_t    index_threads;  /* 索引构建线程数（0/1=单线程） */
} EdbBuildOptions;
```

#### 回调类型

```c
typedef void (*EdbSearchCallback)(const EdbSearchResult* result, void* user_data);
```

### 4.2 API 函数

#### 错误处理

```c
/* 将错误码转换为可读字符串 */
const char* edb_error_message(int code);
```

#### 快照构建

```c
/* 从数组数据构建完整的 edb 快照文件 */
int edb_build_snapshot(const EdbArchiveRecord* archives, uint32_t archive_count,
                       const EdbEntryRecord* entries, uint32_t entry_count,
                       const char* output_path);

/* 从条目流构建完整的 edb 快照文件（支持构建选项） */
int edb_build_snapshot_stream(const EdbArchiveRecord* archives, uint32_t archive_count,
                              EdbEntryStream* entry_stream, uint32_t entry_count,
                              const char* output_path, const EdbBuildOptions* options);
```

- `output_path`：输出文件路径。如果文件已存在则覆盖。
- 内部先写入临时文件（同目录下 `.tmp` 后缀），完成后原子重命名。
- 返回 `EDB_OK` 或错误码。

#### 数据库开关

```c
/* 打开 edb 数据库文件。优先读写模式，失败退回只读 */
int edb_open(const char* path, Edb** out_db);

/* 关闭数据库并释放所有资源 */
void edb_close(Edb* db);
```

- `edb_open` 读取文件头、加载 archive 记录和字符串池到内存、加载 entry 核心记录、加载倒排索引到内存。
- 返回 `EDB_OK`、`EDB_ERR_IO`、`EDB_ERR_FORMAT` 或 `EDB_ERR_MEMORY`。

#### 计数查询

```c
uint32_t edb_count(Ezdb* db);               /* 压缩包总记录数（含非活跃） */
uint32_t edb_active_count(Edb* db);          /* 活跃压缩包记录数 */
uint32_t edb_entry_count(Edb* db);           /* 条目总记录数（含非活跃） */
uint32_t edb_active_entry_count(Edb* db);    /* 活跃条目记录数 */
uint64_t edb_file_size(Edb* db);             /* 数据库文件大小 */
int      edb_stats(Edb* db, EdbStats* out);  /* 详细统计信息 */
```

#### 单条记录查询

```c
/* 按 ID 获取压缩包完整信息 */
int edb_get_archive(Edb* db, uint32_t id, EdbArchiveResult* out);

/* 按 ID 获取条目完整信息 */
int edb_get_entry(Edb* db, uint32_t id, EdbEntryResult* out);

/* 按 (drive_letter, file_ref_number) 查找压缩包 */
int edb_get_archive_by_ref(Edb* db, char drive_letter, uint64_t file_ref_number,
                            EdbArchiveResult* out);
```

- `id` 从 1 开始。
- `out` 指向调用方分配的结构体，函数填充字段（动态分配的字符串需调用方释放）。
- `edb_get_archive_by_ref` 内部使用哈希表，O(1) 查找。
- 找不到返回 `EDB_ERR_NOT_FOUND`。

#### 释放函数

```c
void edb_free_archive_result(EdbArchiveResult* result);
void edb_free_entry_result(EdbEntryResult* result);
void edb_free_search_result(EdbSearchResult* result);
void edb_free_query_page(EdbEntryQueryPage* page);
```

- 释放结果结构体中动态分配的字符串和数组，然后清零结构体。
- 传入 NULL 安全。

#### 搜索

```c
/* 统一搜索：支持压缩包/条目路径搜索，通过 scope 控制搜索范围 */
int edb_search(Edb* db, const char* keyword, uint32_t scope, uint32_t limit,
               EdbSearchCallback callback, void* user_data);

/* 条目分页查询：支持关键词过滤、排序和分页 */
int edb_query_entries(Edb* db, const EdbEntryQuery* query, EdbEntryQueryPage* out);

/* 按 ID 列表批量获取条目 */
int edb_get_entries_batch(Edb* db, const uint32_t* ids, uint32_t count,
                           EdbEntryResult* out);
```

- `edb_search`：通过回调返回匹配结果，最多返回 `limit` 条。结果类型（archive 或 entry）由 `kind` 字段区分。
- `edb_query_entries`：返回分页的条目 ID 列表，调用方随后可用 `edb_get_entry` 或 `edb_get_entries_batch` 获取详情。
- `edb_get_entries_batch`：`out` 指向预分配的 `count` 个 `EdbEntryResult` 数组。

#### 写入事务

```c
/* 开始写入事务 */
int edb_begin_write(Edb* db);

/* 提交写入事务 */
int edb_commit_write(Edb* db);

/* 回滚写入事务 */
int edb_rollback_write(Edb* db);
```

- 事务用于保护 3-phase entry replace 操作。
- `edb_begin_write` 保存当前 active_entry_bits 的快照。
- `edb_commit_write` 清理快照。注意：修改仅在内存中，需要调用 `edb_compact` 持久化到快照文件。
- `edb_rollback_write` 恢复活跃位图快照，撤销所有修改。

#### 压缩包 CRUD

```c
/* 插入或更新单个压缩包（按 drive_letter + file_ref_number 去重） */
int edb_upsert_archive(Edb* db, const EdbArchiveRecord* record, uint32_t* out_id);

/* 批量插入或更新压缩包 */
int edb_upsert_archives(Edb* db, const EdbArchiveRecord* records, uint32_t count,
                         uint32_t* out_ids);

/* 按 (drive_letter, file_ref_number) 删除压缩包（同时标记其所有条目为非活跃） */
int edb_delete_archive_by_ref(Edb* db, char drive_letter, uint64_t file_ref_number);
```

- `edb_upsert_archive`：如果已存在相同 (drive_letter, file_ref_number) 的记录，更新其内容；否则插入新记录。`out_id` 返回记录 ID。
- 这些操作在内存中进行，需要 `edb_compact` 持久化。
- 必须在 `edb_begin_write` / `edb_commit_write` 之间调用。

#### 条目 CRUD

```c
/* 替换指定压缩包下的所有条目（原子操作：标记旧条目非活跃，插入新条目） */
int edb_replace_archive_entries(Edb* db, uint32_t archive_id,
                                 const EdbEntryRecord* entries, uint32_t entry_count);

/* 三段式条目替换（适用于流式场景）：
 *   begin -> append(可多次) -> finish/abort
 */
int edb_begin_replace_archive_entries(Edb* db, uint32_t archive_id);
int edb_append_archive_entries(Edb* db, uint32_t archive_id,
                                const EdbEntryRecord* entries, uint32_t entry_count);
int edb_finish_replace_archive_entries(Edb* db, uint32_t archive_id);
int edb_abort_replace_archive_entries(Edb* db, uint32_t archive_id);
```

- `edb_replace_archive_entries`：等价于 begin + append(全部) + finish。
- 三段式 API 允许分批追加大量条目，避免一次性分配大内存。
- 必须在 `edb_begin_write` / `edb_commit_write` 之间调用。

#### 元数据键值存储

```c
/* 读取元数据（调用方需 free 返回的字符串） */
int edb_get_meta(Edb* db, const char* key, char** out_value);

/* 写入或更新元数据 */
int edb_put_meta(Edb* db, const char* key, const char* value);
```

- 元数据存储在 `.meta` 附属文件中（与数据库文件同目录同名，加 `.meta` 后缀）。
- 简单的 key=value 文本格式，每行一个。
- `edb_get_meta`：找不到返回 `EDB_ERR_NOT_FOUND`，`*out_value` 设为 NULL。

#### 维护

```c
/* 压缩数据库：重写为紧凑快照，回收空间，重建索引 */
int edb_compact(Edb* db);
```

- 将当前内存中的所有活跃数据写入新的快照文件，原子替换原文件。
- 等价于：收集活跃 archive 和 entry → 调用 build_snapshot → 关闭旧文件 → 原子重命名新文件 → 重新打开。

### 4.3 搜索语法

| 语法 | 作用 | 示例 | 说明 |
|---|---|---|---|
| `关键词` | 普通包含搜索 | `发票` | 搜索内容中包含"发票"的结果 |
| `关键词1 关键词2` | 同时包含多个词 | `合同 甲方` | 必须同时包含"合同"和"甲方"，顺序不限 |
| `"短语"` | 精确短语匹配 | `"用户协议"` | 必须连续出现"用户协议" |
| `*` | 通配符：匹配任意长度字符 | `张*明` | 可匹配"张明""张小明""张三丰明" |
| `?` | 通配符：匹配单个字符 | `第?章` | 可匹配"第一章""第2章" |
| `!关键词` | 排除关键词 | `合同 !草稿` | 包含"合同"但不能包含"草稿" |
| `词1 \| 词2` | 或运算 | `发票 \| 收据` | 包含"发票"或"收据" |
| `( ... )` | 分组 | `(发票 \| 收据) 报销` | 组合复杂条件 |

**运算符优先级（从低到高）**：
1. OR（`|`）
2. AND（空格分隔的隐式 AND）
3. NOT（`!`）
4. 原子（关键词、带引号短语、通配符、括号分组）

**大小写**：搜索不区分 ASCII 大小写（A-Z 等同于 a-z），CJK 字符区分。

**特殊字符转义**：在关键词中出现的 `*`、`?`、`!`、`"`、`(`、`)`、`|` 如需作为字面量匹配，暂不支持转义（保留未来扩展）。

## 5. 错误码

```c
#define EDB_OK           0    /* 成功 */
#define EDB_ERR_ARG     -1    /* 无效参数 */
#define EDB_ERR_IO      -2    /* I/O 错误 */
#define EDB_ERR_FORMAT  -3    /* 文件格式错误 */
#define EDB_ERR_MEMORY  -4    /* 内存不足 */
#define EDB_ERR_NOT_FOUND -5  /* 记录未找到 */
#define EDB_ERR_READ_ONLY -6  /* 数据库只读 */
```

## 6. 文件格式

### 6.1 整体布局

```
┌──────────────────────────┐
│ Header (96 bytes)        │ offset 0
├──────────────────────────┤
│ Section data             │
│   ARCHIVE_RECORDS        │
│   ARCHIVE_STRING_POOL    │
│   ENTRY_CORE             │
│   ENTRY_DETAIL_PAGES     │
│   ENTRY_PATH_PAGES       │
│   ENTRY_RAW_BLOBS        │
│   ARCHIVE_POSTINGS       │
│   ARCHIVE_POSTING_INDEX  │
│   ENTRY_POSTINGS         │
│   ENTRY_POSTING_INDEX    │
├──────────────────────────┤
│ Section table            │ array of EdbSectionDesc
└──────────────────────────┘
```

各 section 之间紧密排列（无对齐填充），顺序由构建时决定。Section table 在文件末尾，header 中记录其偏移。

### 6.2 文件头（96 字节）

| 偏移 | 大小 | 字段 | 说明 |
|---|---|---|---|
| 0 | 8 | magic | `"EDB00001"` |
| 8 | 4 | version | `1` (uint32 LE) |
| 12 | 4 | header_size | `96` (uint32 LE) |
| 16 | 4 | flags | 保留，填 0 |
| 20 | 4 | section_count | section 数量 (uint32 LE) |
| 24 | 8 | archive_count | 压缩包总数（含非活跃） |
| 32 | 8 | active_archive_count | 活跃压缩包数 |
| 40 | 8 | entry_count | 条目总数（含非活跃） |
| 48 | 8 | active_entry_count | 活跃条目数 |
| 56 | 8 | section_table_offset | section 表在文件中的偏移 |
| 64 | 4 | checksum | header 前 64 字节的 CRC32 |
| 68 | 28 | reserved | 全零 |
| **合计** | **96** | | |

所有多字节字段使用**小端序**（Little-Endian）。

### 6.3 Section 描述符（48 字节）

Section table 是 `EdbSectionDesc` 数组，按 `section_id` 升序排列。

| 偏移 | 大小 | 字段 | 说明 |
|---|---|---|---|
| 0 | 4 | section_id | section 类型 ID |
| 4 | 4 | flags | 标志位（1 = zlib 压缩） |
| 8 | 8 | offset | 在文件中的起始偏移 |
| 16 | 8 | encoded_size | 磁盘上的字节大小（压缩后） |
| 24 | 8 | raw_size | 原始未压缩大小 |
| 32 | 8 | aux_offset | 辅助数据偏移（分页索引的位置） |
| 40 | 4 | aux_count | 辅助数据条目数（页数） |
| 44 | 4 | page_size | 页大小（非分页 section 为 0） |
| **合计** | **48** | | |

### 6.4 Section 类型

| ID | 名称 | 内容 |
|---|---|---|
| 1 | ARCHIVE_RECORDS | `EdbDiskArchive[]` 固定大小记录数组 |
| 2 | ARCHIVE_STRING_POOL | 压缩包路径字符串（连续 \0 分隔） |
| 3 | ENTRY_CORE | `EdbDiskEntryCore[]` 紧凑核心记录数组 |
| 4 | ENTRY_DETAIL_PAGES | `EdbDiskEntryDetail[]` 分页存储 + 页索引 |
| 5 | ENTRY_PATH_PAGES | UTF-8 路径字符串分页存储 + 页索引 |
| 6 | ENTRY_RAW_BLOBS | 原始路径 blob 分页存储 + 页索引 |
| 7 | ARCHIVE_POSTINGS | 压缩包路径倒排列表 payloads |
| 8 | ARCHIVE_POSTING_INDEX | `EdbDiskIndex[]` 压缩包 gram 索引 |
| 9 | ENTRY_POSTINGS | 条目路径倒排列表 payloads |
| 10 | ENTRY_POSTING_INDEX | `EdbDiskIndex[]` 条目 gram 索引 |

### 6.5 磁盘记录格式

#### EdbDiskArchive（48 字节）

| 偏移 | 大小 | 字段 |
|---|---|---|
| 0 | 4 | name_offset（在 ARCHIVE_STRING_POOL 中的偏移） |
| 4 | 4 | name_len（路径字节长度） |
| 8 | 8 | file_size |
| 16 | 8 | modified_time |
| 24 | 1 | drive_letter |
| 25 | 7 | reserved |
| 32 | 8 | file_ref_number |
| 40 | 8 | usn |

#### EdbDiskEntryCore（12 字节）

| 偏移 | 大小 | 字段 |
|---|---|---|
| 0 | 4 | archive_id |
| 4 | 4 | path_offset（在 ENTRY_PATH_PAGES 中的逻辑偏移） |
| 8 | 4 | path_len（路径字节长度） |

Entry core 记录非常紧凑，仅存储最常用的三个字段，打开数据库时全部加载到内存。

#### EdbDiskEntryDetail（40 字节）

| 偏移 | 大小 | 字段 |
|---|---|---|
| 0 | 8 | compressed_size (int64) |
| 8 | 8 | original_size |
| 16 | 8 | modified_time |
| 24 | 4 | raw_offset（在 ENTRY_RAW_BLOBS 中的逻辑偏移，UINT32_MAX = 无） |
| 28 | 4 | raw_len |
| 32 | 4 | flags（保留） |
| 36 | 4 | reserved |

Detail 记录按需加载（分页 + LRU 缓存）。

#### EdbDiskIndex（32 字节）

| 偏移 | 大小 | 字段 |
|---|---|---|
| 0 | 4 | key（gram key） |
| 4 | 4 | count（posting 列表中的 ID 数量） |
| 8 | 4 | container_type（1=array, 2=range, 3=bitset, 0x80000000=compressed） |
| 12 | 4 | encoded_size |
| 16 | 4 | raw_size |
| 20 | 4 | reserved |
| 24 | 8 | offset（在 POSTINGS section 中的偏移） |

#### EdbDiskPage（24 字节）— 页索引条目

| 偏移 | 大小 | 字段 |
|---|---|---|
| 0 | 8 | offset（在文件中的偏移） |
| 8 | 4 | encoded_size（压缩后大小） |
| 12 | 4 | raw_size（原始大小） |
| 16 | 4 | flags（1 = 压缩） |
| 20 | 4 | reserved |

### 6.6 分页存储格式

ENTRY_DETAIL_PAGES、ENTRY_PATH_PAGES、ENTRY_RAW_BLOBS 使用分页存储。

分页 section 的布局：
```
┌──────────────────────┐
│ Page 0 data          │ ← 可选 zlib 压缩
│ Page 1 data          │
│ ...                  │
│ Page N-1 data        │
├──────────────────────┤
│ EdbDiskPage[0]       │ ← 页索引（aux_offset 指向这里）
│ EdbDiskPage[1]       │
│ ...                  │
│ EdbDiskPage[N-1]     │
└──────────────────────┘
```

- 每页固定大小（ENTRY_DETAIL: 4096 条/页, ENTRY_PATH: 65536 字节/页, ENTRY_RAW_BLOBS: 262144 字节/页）
- 每页独立 zlib 压缩
- Section 描述符的 `aux_offset` 指向页索引数组，`aux_count` 是页数
- 条目 ID 到页的映射：page_index = id / entries_per_page

### 6.7 压缩策略

- **Section 级压缩**：当 raw_size >= 4096 且压缩后节省 >= 256 字节时，整个 section 使用 zlib level 3 压缩，设置 flags 的 bit 0。
- **分页压缩**：每个页独立判断是否压缩。
- **Posting 压缩**：单个 posting 列表可独立压缩（由 container_type 的 bit 31 标记）。

## 7. 内存模型

### 7.1 核心数据结构

```c
/* 内存中的压缩包记录 */
typedef struct {
    uint32_t name_offset;    /* 在 archive_strings 中的偏移 */
    uint32_t name_len;       /* 路径字节长度 */
    uint64_t file_size;
    uint64_t modified_time;
    char     drive_letter;
    uint64_t file_ref_number;
    int64_t  usn;
} EdbArchiveInMem;

/* 内存中的条目核心记录（仅最常用字段，全部加载） */
typedef struct {
    uint32_t archive_id;
    uint32_t path_offset;    /* 逻辑偏移，用于在 ENTRY_PATH_PAGES 中定位 */
    uint32_t path_len;
} EdbEntryCoreInMem;

/* Edb 主结构体（不透明，仅内部使用） */
struct Edb {
    FILE*  fp;               /* 保持打开的文件句柄 */
    char*  path;             /* 数据库文件路径 */
    int    read_only;        /* 是否只读模式 */

    /* === 压缩包数据（全部在内存） === */
    EdbArchiveInMem* archives;
    uint32_t         archive_count;
    uint32_t         archive_cap;
    char*            archive_strings;   /* 连续的路径字符串池 */
    uint64_t         archive_strings_size;

    /* === 条目核心（全部在内存） === */
    EdbEntryCoreInMem* entries;
    uint32_t           entry_count;
    uint32_t           entry_cap;

    /* === 条目详情（分页 + LRU 缓存，按需加载） === */
    EdbDiskPage*    detail_pages;       /* 页索引 */
    uint32_t        detail_page_count;
    EdbPageCache    detail_cache;       /* LRU 页缓存 */

    /* === 条目路径（分页 + LRU 缓存，按需加载） === */
    EdbDiskPage*    path_pages;         /* 页索引 */
    uint32_t        path_page_count;
    EdbPageCache    path_cache;         /* LRU 页缓存 */

    /* === 原始路径 blob（分页 + LRU 缓存，按需加载） === */
    EdbDiskPage*    raw_pages;          /* 页索引 */
    uint32_t        raw_page_count;
    EdbPageCache    raw_cache;          /* LRU 页缓存 */

    /* === 活跃位图 === */
    unsigned char*  active_archive_bits;
    unsigned char*  active_entry_bits;

    /* === 倒排索引（全部在内存，搜索零磁盘 IO） === */
    EdbDiskIndex*   archive_index;
    uint32_t        archive_index_count;
    unsigned char*  archive_postings;   /* posting payloads 连续内存 */
    uint64_t        archive_postings_size;

    EdbDiskIndex*   entry_index;
    uint32_t        entry_index_count;
    unsigned char*  entry_postings;     /* posting payloads 连续内存 */
    uint64_t        entry_postings_size;

    /* === Archive by ref 哈希表 === */
    uint32_t*       ref_hash_buckets;   /* 开放寻址哈希表 */
    uint32_t        ref_hash_cap;

    /* === 3-phase entry replace 状态 === */
    struct EdbEntryReplace* pending_replace;

    /* === 事务快照 === */
    int             txn_active;
    uint32_t        txn_save_entry_count;
    uint32_t        txn_save_active_entry_count;
    unsigned char*  txn_save_active_entry_bits;

    /* === 元数据 === */
    char*           meta_path;
};
```

### 7.2 页缓存（LRU）

```c
#define EDB_CACHE_SLOTS 64   /* 缓存槽数 */

typedef struct {
    uint32_t page_id;         /* 页编号 */
    void*    data;            /* 解压后的页数据 */
    uint32_t tick;            /* 最近访问时间戳 */
    int      valid;           /* 槽位是否有效 */
} EdbCacheSlot;

typedef struct {
    EdbCacheSlot slots[EDB_CACHE_SLOTS];
    uint32_t    tick_counter; /* 单调递增时间戳 */
} EdbPageCache;
```

- 缓存策略：固定大小 LRU，线性扫描淘汰（64 槽足够小，线性扫描可接受）。
- 命中：更新 tick，返回数据指针。
- 未中：淘汰 tick 最小的槽位，从磁盘读取并解压对应页。

### 7.3 Archive by Ref 哈希表

打开数据库时构建，将 `(drive_letter, file_ref_number)` 映射到 archive ID。

```c
/* key: (drive_letter << 56) | file_ref_number */
/* value: archive_id (1-based) */
```

使用开放寻址哈希表，murmur3 finalizer mix 做哈希，负载因子 <= 0.7。

## 8. 倒排索引与搜索引擎

### 8.1 N-gram 策略

对文本提取 1-gram、2-gram、3-gram，对中文和英文都有效。

**分词规则**：
- 文本按 UTF-8 codepoint 分割为 token 序列
- ASCII 字母（A-Z, a-z）折叠为小写
- 路径分隔符（`/`、`\`）和点号（`.`）视为 token 边界（不参与 gram 构造）
- 其他字符（CJK、数字、下划线等）保留原样

**Gram 提取**：
- 对长度为 N 的 token 序列，提取：
  - 所有 1-gram（每个单独 token）
  - 所有 2-gram（相邻两 token 组合）
  - 所有 3-gram（相邻三 token 组合）
- 示例：文本 "发票.pdf" → tokens: ["发票", "pdf"] → 1-grams: ["发票", "pdf"], 2-grams: ["发票pdf"]

**Gram key 编码**：
- 如果 gram 的原始 UTF-8 字节 <= 3 字节：`inline_key = (token_count << 24) | utf8_bytes`，bit31 = 0
- 如果 > 3 字节：`hashed_key = 0x80000000 | (token_count << 24) | (fnv1a(utf8_bytes) & 0x00FFFFFF)`，bit31 = 1

### 8.2 Posting Builder

构建倒排索引的核心数据结构。

```c
typedef struct {
    uint32_t key;          /* gram key */
    uint32_t* ids;         /* 排序的 ID 列表 */
    uint32_t count;        /* 当前 ID 数量 */
    uint32_t cap;          /* 容量 */
    uint32_t next;         /* 哈希链：下一个 bucket 索引 */
    uint32_t fill_mode;    /* 0=数组, 1=bitset */
    uint32_t fill_bytes;   /* bitset 模式时的字节大小 */
    uint32_t last_id;      /* count 阶段去重用 */
} EdbPostingEntry;

typedef struct {
    EdbPostingEntry* entries;
    uint32_t entry_count;
    uint32_t entry_cap;
    uint32_t* buckets;     /* 哈希桶 */
    uint32_t bucket_count;
    uint32_t* id_block;    /* prepare 阶段分配的连续 ID 存储 */
} EdbPostingBuilder;
```

**三阶段构建**：

1. **Count 阶段**：遍历所有文本，对每个文本提取 gram keys，将 (key, id) 对添加到 builder。此阶段只计数不分配 ID 存储。
2. **Prepare 阶段**：分配连续的 `id_block`。高密度 posting（count >= universe/16）使用 bitset 模式。
3. **Fill 阶段**：再次遍历所有文本，将 ID 填充到预分配的存储中。

### 8.3 自适应 Posting 编码

写入磁盘时，每个 posting 列表自动选择最优编码：

| 编码类型 | 值 | 格式 | 选择条件 |
|---|---|---|---|
| ARRAY | 1 | delta varint 编码的排序 ID 列表 | 默认 |
| RANGE | 2 | (start, count) 对，表示连续 ID 段 | range_count <= count/2 且 range_size < array_size |
| BITSET | 3 | 每 bit 代表一个 ID | count >= universe/16 且 bitset_size < encoded_size |

任何编码类型都可以额外 zlib 压缩（container_type 的 bit31 = 1），条件：压缩后节省 >= 256 字节。

### 8.4 多线程构建

Entry posting 构建支持多线程并行：

- **并行 Count**：将 entry 按 archive 分成 N 个范围，每个线程独立 count，最后合并到全局 builder。
- **并行 Fill**：每个线程在全局 id_block 的分片范围内填充，bitset 模式使用原子 OR。
- 线程抽象：使用 C11 `<threads.h>`，如果不支持则条件编译使用 `_beginthreadex`（Windows）或 `pthread`（POSIX）。

### 8.5 查询解析器

递归下降解析器，将搜索关键词转为 AST。

**AST 节点类型**：

```c
typedef enum {
    EDB_QUERY_TERM     = 1,  /* 普通关键词 */
    EDB_QUERY_WILDCARD = 2,  /* 包含 * 或 ? 的通配符 */
    EDB_QUERY_NOT      = 3,  /* 取反 */
    EDB_QUERY_AND      = 4,  /* 逻辑与 */
    EDB_QUERY_OR       = 5   /* 逻辑或 */
} EdbQueryNodeType;

typedef struct EdbQueryNode {
    EdbQueryNodeType type;
    char*  text;             /* TERM/WILDCARD 的文本 */
    size_t text_len;
    struct EdbQueryNode* left;
    struct EdbQueryNode* right;
} EdbQueryNode;
```

**解析规则**：
- `|` 两侧的子表达式构成 OR 节点
- 空格分隔的多个词构成 AND 节点
- `!` 前缀构成 NOT 节点
- `"..."` 内的内容作为单个 TERM 节点
- `( ... )` 内的表达式递归解析
- 包含 `*` 或 `?` 的词成为 WILDCARD 节点

### 8.6 查询评估

1. **候选集生成**：遍历 AST，对每个 TERM/WILDCARD 节点：
   - 提取最长字面子串的 gram keys
   - 从内存中的 posting index 查找对应 posting
   - 交集得到候选 ID 集合
2. **AND/OR 组合**：对子节点结果做交集/并集
3. **NOT 处理**：从候选集中减去 NOT 子树的匹配
4. **通配符后过滤**：对通配符节点，将候选集中的每个 ID 对应的路径与通配符模式匹配
5. **Scope 过滤**：根据 scope 参数，检查匹配的路径字段
6. **排序和分页**：对结果按指定列排序，应用 offset/limit

### 8.7 搜索执行流程

**edb_search**：
1. 解析 keyword 为 AST
2. 根据 scope 确定搜索 archive 还是 entry 还是两者
3. 对 archive：从 archive_postings + archive_index 中查找匹配的 archive ID
4. 对 entry：从 entry_postings + entry_index 中查找匹配的 entry ID
5. 对 COMBINED_PATH：匹配 archive_path + "\n" + entry_path 的拼接
6. 通过回调返回结果，最多 limit 条

**edb_query_entries**：
1. 解析 keyword 为 AST
2. 从 entry index 获取候选 entry ID 集合
3. 按 sort_column 排序（快速排序）
4. 应用 offset/limit 分页
5. 返回 EdbEntryQueryPage（包含 total_count 和 ids 数组）

## 9. 快照构建流程

### 9.1 构建步骤

```
输入: archives[], entry_count, entry_stream(或 entries[]), output_path, options
                                                │
                                                ▼
                                    ┌─ 1. 创建临时文件 ─┐
                                                │
                                                ▼
                              ┌─ 2. 字符串去重（archive paths）──┐
                              │    FNV-1a 哈希 + 开放寻址        │
                              │    输出: string_pool[]           │
                              └──────────────────────────────────┘
                                                │
                                                ▼
                          ┌─ 3. 写 ARCHIVE_RECORDS section ──────┐
                          │    序列化 EdbDiskArchive[]             │
                          │    可选 zlib 压缩                      │
                          └───────────────────────────────────────┘
                                                │
                                                ▼
                          ┌─ 4. 写 ARCHIVE_STRING_POOL section ──┐
                          └───────────────────────────────────────┘
                                                │
                                                ▼
                          ┌─ 5. 流式收集 entries ─────────────────┐
                          │    写 ENTRY_CORE (连续数组)            │
                          │    写 ENTRY_DETAIL_PAGES (分页)        │
                          │    写 ENTRY_PATH_PAGES (分页)          │
                          │    写 ENTRY_RAW_BLOBS (分页)           │
                          └───────────────────────────────────────┘
                                                │
                                                ▼
                          ┌─ 6. 构建 archive 倒排索引 ────────────┐
                          │    count → prepare → fill → write     │
                          │    写 ARCHIVE_POSTINGS                 │
                          │    写 ARCHIVE_POSTING_INDEX            │
                          └───────────────────────────────────────┘
                                                │
                                                ▼
                          ┌─ 7. 构建 entry 倒排索引 ──────────────┐
                          │    可并行 count（按 archive 分范围）    │
                          │    count → prepare → fill → write     │
                          │    写 ENTRY_POSTINGS                   │
                          │    写 ENTRY_POSTING_INDEX              │
                          └───────────────────────────────────────┘
                                                │
                                                ▼
                          ┌─ 8. 写 section table + header ────────┐
                          │    回填 header 到文件开头               │
                          └───────────────────────────────────────┘
                                                │
                                                ▼
                          ┌─ 9. 原子重命名 ──────────────────────┐
                          │    tmp → output_path                   │
                          └───────────────────────────────────────┘
```

### 9.2 字符串去重

构建时对 archive 路径做去重，减少存储空间：

- 使用 FNV-1a 哈希 + 开放寻址的字符串哈希表
- 对每个路径字符串，查找是否已存在：
  - 存在：复用已有的 offset
  - 不存在：追加到 string pool，记录 offset
- 许多 archive 共享目录前缀，去重可节省 30-50% 的字符串存储

Entry 路径**不做去重**（路径通常各不相同），直接按顺序写入分页。

### 9.3 Entry 分页写入

Entry 数据量大，采用分页写入策略：

1. 维护三个分页写入器：detail、path、raw
2. 每个 writer 缓冲固定大小的页，满一页就写磁盘
3. 记录每页的 (offset, encoded_size, raw_size, flags)
4. 所有 entry 写完后，将页索引追加到 section 末尾

```c
typedef struct {
    FILE*         fp;            /* 输出文件 */
    uint32_t      page_size;     /* 每页字节数或条目数 */
    uint32_t      items_per_page;/* 每页条目数（固定大小记录时） */
    uint8_t*      buf;           /* 当前页缓冲区 */
    uint32_t      buf_used;      /* 当前页已用字节数 */
    EdbDiskPage*  pages;         /* 页索引数组 */
    uint32_t      page_count;    /* 已写出的页数 */
    uint32_t      page_cap;
} EdbPagedWriter;
```

## 10. 写入路径

### 10.1 3-Phase Entry Replace

用于流式替换某个压缩包下的所有条目。

```c
typedef struct EdbEntryReplace {
    uint32_t archive_id;

    /* 旧 entry 的活跃位图快照（用于 abort 恢复） */
    unsigned char* saved_bits;
    uint32_t       saved_bits_len;

    /* 新追加的 entry 核心 + 详情 */
    EdbEntryCoreInMem* new_cores;
    uint32_t           new_core_count;
    uint32_t           new_core_cap;

    /* 新 entry 的详情数据（compressed_size, original_size, etc.） */
    int64_t*  new_compressed_sizes;
    uint64_t* new_original_sizes;
    uint64_t* new_modified_times;

    /* 新 entry 的路径字符串 */
    char*     new_paths;
    uint64_t  new_paths_size;
    uint64_t  new_paths_cap;

    /* 新 entry 的 raw blob */
    uint8_t*  new_raw_blobs;
    uint32_t  new_raw_blob_size;
    uint32_t  new_raw_blob_cap;
    uint32_t* new_raw_lens;
} EdbEntryReplace;
```

**流程**：

1. **edb_begin_replace_archive_entries(db, archive_id)**：
   - 保存当前 `active_entry_bits` 中属于此 archive 的所有 entry 的活跃状态
   - 将这些 entry 标记为非活跃
   - 分配 `EdbEntryReplace` 结构

2. **edb_append_archive_entries(db, archive_id, entries, count)**：
   - 将 entry 记录追加到 `EdbEntryReplace` 的缓冲区
   - 拷贝 path 和 raw blob 数据

3. **edb_finish_replace_archive_entries(db, archive_id)**：
   - 将新 entry 追加到 `db->entries` 数组
   - 将新 path 追加到 path 分页写入器（直接写入新页）
   - 更新 `db->entry_count` 和 `db->active_entry_count`
   - 在 `active_entry_bits` 中标记新 entry 为活跃
   - 增量更新内存中的 entry posting index
   - 释放 `EdbEntryReplace`

4. **edb_abort_replace_archive_entries(db, archive_id)**：
   - 从快照恢复活跃位图
   - 释放 `EdbEntryReplace`

### 10.2 事务 API

简化实现：

- **edb_begin_write(db)**：
  - 断言 `txn_active == 0`
  - 拷贝 `active_entry_bits` → `txn_save_active_entry_bits`
  - 保存 `entry_count` → `txn_save_entry_count`
  - 设置 `txn_active = 1`

- **edb_commit_write(db)**：
  - 断言 `txn_active == 1`
  - 释放 `txn_save_active_entry_bits`
  - 设置 `txn_active = 0`

- **edb_rollback_write(db)**：
  - 断言 `txn_active == 1`
  - 恢复 `active_entry_bits` ← `txn_save_active_entry_bits`
  - 恢复 `entry_count` ← `txn_save_entry_count`
  - 如果有 pending_replace，执行 abort
  - 释放快照
  - 设置 `txn_active = 0`

注意：commit 后数据仅在内存，需要调用 `edb_compact()` 才能持久化到快照文件。

### 10.3 Upsert / Delete

- **edb_upsert_archive**：
  - 在 ref 哈希表中查找 (drive_letter, file_ref_number)
  - 找到：更新 archive 记录的字段
  - 未找到：追加到 archives 数组，分配新 ID

- **edb_delete_archive_by_ref**：
  - 在 ref 哈希表中查找
  - 在 active_archive_bits 中标记为非活跃
  - 遍历 entries 数组，将 archive_id 匹配的 entry 标记为非活跃

- 这些操作必须在 `edb_begin_write` / `edb_commit_write` 之间调用。

### 10.4 Compact

```c
int edb_compact(Edb* db) {
    // 1. 收集所有活跃的 archive
    // 2. 收集所有活跃的 entry
    // 3. 构建 EdbArchiveRecord[] 和 EdbEntryRecord[] 数组
    // 4. 调用 edb_build_snapshot 写入临时文件
    // 5. 关闭旧文件
    // 6. 原子重命名临时文件到原路径
    // 7. 重新 edb_open
}
```

## 11. 元数据存储

元数据存储在独立的 `.meta` 文件中。

- 文件路径：`{db_path}.meta`（例如 `data.edb` → `data.edb.meta`）
- 格式：每行一个 `key=value` 对，UTF-8 文本
- 读取：扫描文件，找到匹配 key 的行，返回 value 部分
- 写入：重写整个文件（先写临时文件再重命名）
- `edb_get_meta`：找到返回 `EDB_OK`，找不到返回 `EDB_ERR_NOT_FOUND`
- `edb_put_meta`：key 已存在则更新，不存在则追加

## 12. 模块划分与实现顺序

### 12.1 模块职责

| 模块 | 文件 | 职责 |
|---|---|---|
| **edb.h** | edb.h | 公开 API 声明、类型定义、常量定义 |
| **edb_util** | edb_util.c, edb_util.h | 跨平台工具函数：文件操作、线程抽象、哈希、位图、字符串 |
| **edb_format** | edb_format.c, edb_format.h | 文件格式读写：header、section table、压缩/解压、分页读写 |
| **edb_postings** | edb_postings.c, edb_postings.h | N-gram 分词、posting builder、自适应编码、posting 读写、并行构建 |
| **edb_build** | edb_build.c, edb_build.h | 快照构建：字符串去重、序列化、分页写入、调用 postings 模块构建索引 |
| **edb_search** | edb_search.c, edb_search.h | 查询解析器（AST）、搜索评估、posting 交集、通配符匹配、分页查询 |
| **edb** | edb.c | 主模块：open/close、get/count/stats、事务、3-phase replace、upsert/delete、compact、meta |

### 12.2 依赖关系

```
edb.h ──────────────────────────────────────────────────────
  │
  ├── edb.c
  │     ├── edb_format
  │     ├── edb_build
  │     ├── edb_search
  │     └── edb_util
  │
  ├── edb_build
  │     ├── edb_format
  │     ├── edb_postings
  │     └── edb_util
  │
  ├── edb_search
  │     ├── edb_postings
  │     └── edb_util
  │
  ├── edb_postings
  │     └── edb_util
  │
  ├── edb_format
  │     └── edb_util
  │
  └── edb_util
```

### 12.3 实现顺序

1. **edb_util** (~300 行) — 无依赖，可独立测试
2. **edb_format** (~400 行) — 依赖 edb_util
3. **edb_postings** (~900 行) — 依赖 edb_util
4. **edb_build** (~700 行) — 依赖 edb_format + edb_postings
5. **edb.c (open/close/get)** (~600 行) — 依赖 edb_format
6. **edb_search** (~800 行) — 依赖 edb_postings
7. **edb.c (writes + compact + meta)** (~500 行) — 依赖 edb_build

## 13. edb_util 工具模块详细设计

### 13.1 线程抽象

```c
/* 跨平台线程 */
typedef struct {
    /* 平台相关句柄 */
#if defined(_WIN32)
    void* handle;
#else
    /* pthread_t */
    unsigned long id;
#endif
} EdbThread;

typedef void* (*edb_thread_fn)(void* arg);

int  edb_thread_create(EdbThread* t, edb_thread_fn fn, void* arg);
int  edb_thread_join(EdbThread* t);
```

### 13.2 工具函数

```c
/* FNV-1a 哈希 */
uint32_t edb_fnv1a(const void* data, size_t len);

/* Murmur3 finalizer mix */
uint32_t edb_murmur3_final(uint32_t h);

/* 位图操作 */
int  edb_bit_get(const unsigned char* bits, uint32_t idx);
void edb_bit_set(unsigned char* bits, uint32_t idx);
void edb_bit_clear(unsigned char* bits, uint32_t idx);

/* 动态数组增长 */
int  edb_ensure_cap(void** arr, uint32_t* cap, uint32_t needed, uint32_t elem_size);

/* UTF-8 工具 */
uint32_t edb_utf8_char_len(const uint8_t* s, size_t remain);
uint8_t  edb_ascii_fold(uint8_t ch);  /* A-Z → a-z */

/* 文件工具 */
int64_t edb_file_size(FILE* fp);
int     edb_file_sync(FILE* fp);      /* fflush + 平台 sync */
int     edb_rename_atomic(const char* old_path, const char* new_path);

/* 变长整数编码 */
int     edb_write_varuint(uint8_t* buf, uint32_t val);
int     edb_read_varuint(const uint8_t* buf, uint32_t* val);
int     edb_varuint_size(uint32_t val);
```

## 14. 测试策略

### 14.1 单元测试

- **edb_util**：哈希正确性、位图操作、varint 编解码、UTF-8 字符长度
- **edb_postings**：gram key 生成、posting builder add/count/fill、自适应编码选择、posting 读写往返
- **edb_search**：查询解析器（各种语法组合）、通配符匹配
- **edb_format**：header 读写、section table 读写、分页读写

### 14.2 集成测试

1. 构建：创建测试数据 → build_snapshot → 验证文件头和 section 完整性
2. 查询：打开数据库 → get_archive/get_entry → 验证数据正确性
3. 搜索：构建包含中英文路径的数据库 → 搜索验证结果
4. 写入：打开 → begin_write → 3-phase replace → commit → 验证数据
5. Compact：写入数据 → compact → 重新打开验证
6. 原子性：begin_write → 修改 → rollback → 验证数据不变

### 14.3 兼容性测试

修改 `src/index_store.cpp` 将 `ezdb_*` 调用替换为 `edb_*`，运行现有测试确认通过。

## 15. 与现有代码对接

### 15.1 API 映射

所有 `Ezdb*` 类型名和 `ezdb_*` 函数名直接替换为 `Edb*` 和 `edb_*`，语义完全一致。

| 旧 (ezdb) | 新 (edb) |
|---|---|
| `Ezdb` | `Edb` |
| `EzdbFileRecord` | `EdbFileRecord` |
| `EzdbArchiveRecord` | `EdbArchiveRecord` |
| `EzdbEntryRecord` | `EdbEntryRecord` |
| `EzdbEntryStream` | `EdbEntryStream` |
| `EzdbBuildOptions` | `EdbBuildOptions` |
| `EzdbArchiveResult` | `EdbArchiveResult` |
| `EzdbEntryResult` | `EdbEntryResult` |
| `EzdbSearchV2Result` | `EdbSearchResult` |
| `EzdbEntryQuery` | `EdbEntryQuery` |
| `EzdbEntryQueryPage` | `EdbEntryQueryPage` |
| `EzdbStats` | `EdbStats` |
| `EZDB_SEARCH_*` | `EDB_SEARCH_*` |
| `EZDB_BUILD_*` | `EDB_BUILD_*` |
| `EZDB_OK` / `EZDB_ERR_*` | `EDB_OK` / `EDB_ERR_*` |
| `ezdb_build_snapshot` | `edb_build_snapshot` |
| `ezdb_build_snapshot_stream_entries` | `edb_build_snapshot_stream` |
| `ezdb_search` | `edb_search` |
| ... (所有 ezdb_* 函数) | edb_* 对应函数 |

### 15.2 index_store.cpp 修改要点

- `#include "ezdb.h"` → `#include "edb.h"`
- 所有 `Ezdb*` 类型 → `Edb*`
- 所有 `ezdb_*()` → `edb_*()`
- `EzdbSearchV2Result` → `EdbSearchResult`
- `EzdbSearchV2Callback` → `EdbSearchCallback`
- `ezdb_free_search_v2_result` → `edb_free_search_result`
- `ezdb_build_snapshot_stream_entries` → `edb_build_snapshot_stream`
