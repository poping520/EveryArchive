#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Ezdb Ezdb;

/* ===== 压缩包记录 ===== */

/* 表示一个磁盘上的普通文件，包含 NTFS 文件系统级别的标识信息 */
typedef struct EzdbFileRecord {
    char drive_letter;          /* 所在驱动器盘符（如 'C'） */
    uint64_t file_ref_number;   /* NTFS 文件引用编号，用于唯一标识文件 */
    int64_t usn;                /* NTFS USN（Update Sequence Number）日志序号 */
    const char* file_path;      /* 压缩包完整路径 */
    uint64_t file_size;         /* 压缩包文件大小 */
    uint64_t modified_time;     /* 压缩包最后修改时间 */
} EzdbFileRecord;

// 表示一个压缩包文件（如 .zip / .7z / .rar）
typedef EzdbFileRecord EzdbArchiveRecord;

/* ===== 压缩包内部条目记录 ===== */

/* 表示压缩包内的一个条目（文件或目录），由外部调用方填充后写入 ezdb */
typedef struct EzdbEntryRecord {
    uint32_t archive_id;        /* 所属压缩包的 ID */
    const char* entry_path;     /* 条目路径（UTF-8 字符串） */
    const void* entry_raw_path; /* 条目原始路径（压缩包内部编码，如 UTF-16） */
    uint32_t entry_raw_path_len;/* 原始路径字节长度 */
    int64_t compressed_size;    /* 条目压缩后大小（-1 表示无压缩信息） */
    uint64_t original_size;     /* 条目原始大小 */
    uint64_t modified_time;     /* 条目修改时间 */
} EzdbEntryRecord;

/* ===== 条目流式读取接口 ===== */

/*
 * 流式迭代器接口，用于大规模条目数据的逐条读取。
 * 调用方实现此接口以提供条目数据，避免一次性加载所有条目到内存。
 */
typedef struct EzdbEntryStream {
    void* user_data;            /* 调用方自定义上下文 */
    int (*reset)(void* user_data);  /* 重置迭代器到起始位置 */
    int (*reset_range)(void* user_data, uint32_t archive_begin, uint32_t archive_end); /* 重置到指定压缩包范围 */
    int (*next)(void* user_data, EzdbEntryRecord* out_record);  /* 读取下一条记录 */
    int (*open_range)(void* user_data, uint32_t archive_begin, uint32_t archive_end, struct EzdbEntryStream* out_stream); /* 打开子范围流 */
    void (*close_range)(struct EzdbEntryStream* stream);  /* 关闭子范围流 */
} EzdbEntryStream;

/* ===== 构建选项 ===== */

#define EZDB_BUILD_ENTRY_INDEX 0x01u          /* 为条目路径构建倒排索引 */
#define EZDB_BUILD_POSTING_COMPRESSION 0x02u  /* 对倒排列表启用 zlib 压缩 */
#define EZDB_BUILD_DEFAULT_FLAGS (EZDB_BUILD_ENTRY_INDEX | EZDB_BUILD_POSTING_COMPRESSION)

#define EZDB_WRITE_BULK 1u   /* 批量写入模式：缓冲条目，在 commit 时统一构建索引 */

/* 构建快照时的配置选项 */
typedef struct EzdbBuildOptions {
    const char* temp_dir;       /* 临时文件目录（NULL 则使用输出路径同目录） */
    uint32_t memory_limit_mb;   /* 内存限制（MB，0 使用默认 512） */
    uint32_t flags;             /* 构建标志位（EZDB_BUILD_* 组合） */
    uint32_t log_level;         /* 日志级别（0=静默） */
    uint32_t index_threads;     /* 索引构建线程数（0/1=单线程） */
    uint32_t zip_threads;       /* 压缩线程数（0=自动） */
} EzdbBuildOptions;

/* ===== 查询结果结构体 ===== */

/* 压缩包查询结果，包含 NTFS 标识信息 */
typedef struct EzdbArchiveResult {
    uint32_t id;                /* 压缩包 ID */
    char drive_letter;          /* 驱动器盘符 */
    uint64_t file_ref_number;   /* NTFS 文件引用编号 */
    int64_t usn;                /* NTFS USN 日志序号 */
    char* file_path;            /* 压缩包完整路径（调用方需 free） */
    uint64_t file_size;         /* 压缩包文件大小 */
    uint64_t modified_time;     /* 修改时间 */
} EzdbArchiveResult;

/* 压缩包内条目查询结果 */
typedef struct EzdbEntryResult {
    uint32_t id;                /* 条目 ID */
    uint32_t archive_id;        /* 所属压缩包 ID */
    char* archive_path;         /* 所属压缩包路径（调用方需 free） */
    char* entry_path;           /* 条目路径（调用方需 free） */
    void* entry_raw_path;       /* 原始路径数据（调用方需 free） */
    uint32_t entry_raw_path_len;/* 原始路径字节长度 */
    int64_t compressed_size;    /* 压缩后大小 */
    uint64_t original_size;     /* 原始大小 */
    uint64_t modified_time;     /* 修改时间 */
} EzdbEntryResult;

/* 搜索结果类型：区分压缩包匹配和条目匹配 */
typedef enum EzdbSearchKind {
    EZDB_RESULT_ARCHIVE = 1,    /* 匹配的是压缩包路径 */
    EZDB_RESULT_ENTRY = 2       /* 匹配的是条目路径 */
} EzdbSearchKind;

/* 统一的 V2 搜索结果，可表示压缩包或条目匹配 */
typedef struct EzdbSearchV2Result {
    EzdbSearchKind kind;        /* 结果类型（压缩包 / 条目） */
    uint32_t id;                /* 记录 ID */
    uint32_t archive_id;        /* 所属压缩包 ID（仅条目类型有效） */
    char drive_letter;          /* 驱动器盘符 */
    uint64_t file_ref_number;   /* NTFS 文件引用编号 */
    int64_t usn;                /* NTFS USN 日志序号 */
    char* archive_path;         /* 压缩包路径（调用方需 free） */
    char* entry_path;           /* 条目路径（调用方需 free） */
    void* entry_raw_path;       /* 原始路径数据（调用方需 free） */
    uint32_t entry_raw_path_len;/* 原始路径字节长度 */
    int64_t compressed_size;    /* 压缩后大小 */
    uint64_t original_size;     /* 原始大小 */
    uint64_t file_size;         /* 文件大小（压缩包级别） */
    uint64_t modified_time;     /* 修改时间 */
} EzdbSearchV2Result;

/* 搜索范围标志位：控制搜索匹配的字段 */
#define EZDB_SEARCH_ARCHIVE_PATH  0x01u  /* 搜索压缩包路径 */
#define EZDB_SEARCH_ENTRY_PATH    0x02u  /* 搜索条目路径 */
#define EZDB_SEARCH_COMBINED_PATH 0x04u  /* 搜索组合路径（压缩包路径 + 条目路径） */
#define EZDB_SEARCH_ALL (EZDB_SEARCH_ARCHIVE_PATH | EZDB_SEARCH_ENTRY_PATH) /* 搜索所有路径 */

/* ===== 数据库统计信息 ===== */

/* 数据库各部分的记录数量和存储空间占用统计 */
typedef struct EzdbStats {
    uint32_t record_count;      /* 压缩包总记录数（含已删除） */
    uint32_t active_count;      /* 活跃压缩包记录数 */
    uint32_t entry_count;       /* 条目总记录数（含已删除） */
    uint32_t active_entry_count;/* 活跃条目记录数 */
    uint32_t base_entry_count;  /* 基础快照中的条目数 */
    uint32_t delta_entry_count; /* 通过增量日志追加的条目数 */
    uint64_t file_size;         /* 数据库文件总大小 */
    uint64_t delta_size;        /* 增量日志占用大小 */
    uint64_t records_size;      /* 压缩包记录段占用大小 */
    uint64_t dirs_size;         /* 目录记录段占用大小 */
    uint64_t names_size;        /* 字符串池占用大小 */
    uint64_t archive_meta_size; /* 压缩包元数据段占用大小 */
    uint64_t entry_records_size;/* 条目核心记录段占用大小 */
    uint64_t raw_blob_size;     /* 原始路径数据段占用大小 */
    uint64_t index_size;        /* 所有索引段总大小 */
    uint64_t postings_size;     /* 倒排列表段总大小 */
} EzdbStats;

/* ===== 条目分页查询 ===== */

/* 条目分页查询参数 */
typedef struct EzdbEntryQuery {
    const char* keyword;        /* 搜索关键词（支持通配符、AND/OR/NOT 语法） */
    uint32_t scope;             /* 搜索范围（EZDB_SEARCH_* 标志位） */
    int sort_column;            /* 排序列（0=默认, 1=路径, 2=大小, 3=时间） */
    int sort_ascending;         /* 升序排序标志 */
    uint32_t offset;            /* 分页偏移量 */
    uint32_t limit;             /* 每页返回数量 */
} EzdbEntryQuery;

/* 条目分页查询结果 */
typedef struct EzdbEntryQueryPage {
    uint64_t total_count;       /* 满足条件的总条目数 */
    uint32_t returned_count;    /* 本页返回的条目数量 */
    uint32_t* ids;              /* 本页返回的条目 ID 数组（调用方需 free） */
} EzdbEntryQueryPage;

/* ===== 回调函数类型 ===== */

/* V2 统一搜索结果回调 */
typedef void (*EzdbSearchV2Callback)(const EzdbSearchV2Result* result, void* user_data);

/* ===== 快照构建 API ===== */

/* 从数组数据构建完整的 ezdb 快照文件（旧版，无构建选项） */
int ezdb_build_snapshot(const EzdbArchiveRecord* archives,
                        uint32_t archive_count,
                        const EzdbEntryRecord* entries,
                        uint32_t entry_count,
                        const char* output_ezdb);

/* 从条目流构建完整的 ezdb 快照文件（扩展版，支持构建选项） */
int ezdb_build_snapshot_stream_entries(const EzdbArchiveRecord* archives,
                                          uint32_t archive_count,
                                          EzdbEntryStream* entry_stream,
                                          uint32_t entry_count,
                                          const char* output_ezdb,
                                          const EzdbBuildOptions* options);

/* ===== 数据库开关 ===== */

/* 打开 ezdb 数据库文件。优先以读写模式打开，失败则退回只读模式 */
int ezdb_open(const char* path, Ezdb** out_db);

/* 关闭数据库并释放所有资源 */
void ezdb_close(Ezdb* db);

/* ===== 计数查询 ===== */

uint32_t ezdb_count(Ezdb* db);              /* 压缩包总记录数（含已删除） */
uint32_t ezdb_active_count(Ezdb* db);       /* 活跃压缩包记录数 */
uint32_t ezdb_entry_count(Ezdb* db);        /* 条目总记录数（含已删除） */
uint32_t ezdb_active_entry_count(Ezdb* db); /* 活跃条目记录数 */
uint64_t ezdb_file_size(Ezdb* db);          /* 数据库文件当前大小 */

/* 获取数据库各部分详细统计信息 */
int ezdb_stats(Ezdb* db, EzdbStats* out_stats);

/* ===== 单条记录查询 ===== */

/* 按 ID 获取压缩包完整信息（含 NTFS 标识） */
int ezdb_get_archive(Ezdb* db, uint32_t id, EzdbArchiveResult* out_result);

/* 按 ID 获取条目完整信息（含所属压缩包路径和原始路径） */
int ezdb_get_entry(Ezdb* db, uint32_t id, EzdbEntryResult* out_result);

/* 按驱动器盘符和 NTFS 文件引用编号查找压缩包 */
int ezdb_get_archive_by_ref(Ezdb* db, char drive_letter, uint64_t file_ref_number, EzdbArchiveResult* out_result);

/* 释放各种结果结构体 */
void ezdb_free_archive_result(EzdbArchiveResult* result);
void ezdb_free_entry_result(EzdbEntryResult* result);
void ezdb_free_search_v2_result(EzdbSearchV2Result* result);
void ezdb_free_entry_query_page(EzdbEntryQueryPage* page);

/* ===== 搜索 API ===== */

/* V2 统一搜索：支持压缩包/条目路径搜索，通过 scope 控制搜索范围 */
int ezdb_search(Ezdb* db, const char* keyword, uint32_t scope, uint32_t limit, EzdbSearchV2Callback callback, void* user_data);

/* 条目分页查询：支持关键词过滤、排序和分页 */
int ezdb_query_entries(Ezdb* db, const EzdbEntryQuery* query, EzdbEntryQueryPage* out_page);

/* 按 ID 列表批量获取条目详情 */
int ezdb_get_entries_batch(Ezdb* db, const uint32_t* ids, uint32_t count, EzdbEntryResult* out_results);

/* ===== 写入事务 API ===== */

/*
 * 写入事务用于批量修改数据库。支持两种模式：
 * - 普通模式（flags=0）：每条操作立即写盘
 * - 批量模式（flags=EZDB_WRITE_BULK）：缓冲所有修改，在 commit 时统一构建索引并写盘
 */

/* 开始写入事务 */
int ezdb_begin_write(Ezdb* db, uint32_t flags);

/* 提交写入事务，将所有修改持久化到磁盘 */
int ezdb_commit_write(Ezdb* db);

/* 回滚写入事务，撤销自 begin_write 以来的所有修改 */
int ezdb_rollback_write(Ezdb* db);

/* ===== 压缩包与条目 CRUD ===== */

/* 插入或更新单个压缩包（按 drive_letter + file_ref_number 去重） */
int ezdb_upsert_archive(Ezdb* db, const EzdbArchiveRecord* record, uint32_t* out_id);

/* 批量插入或更新压缩包 */
int ezdb_upsert_archives(Ezdb* db, const EzdbArchiveRecord* records, uint32_t count, uint32_t* out_ids);

/* 按驱动器盘符和文件引用编号删除压缩包（同时停用其下所有条目） */
int ezdb_delete_archive_by_ref(Ezdb* db, char drive_letter, uint64_t file_ref_number);

/* 替换指定压缩包下的所有条目（原子操作：先停用旧条目再插入新条目） */
int ezdb_replace_archive_entries(Ezdb* db, uint32_t archive_id, const EzdbEntryRecord* entries, uint32_t entry_count);

/* 三段式条目替换 API（适用于流式场景）：
 *   begin -> append(可多次) -> finish/abort
 * 在 begin 和 finish 之间，条目被逐步追加；finish 时统一构建索引并激活。
 */
int ezdb_begin_replace_archive_entries(Ezdb* db, uint32_t archive_id);
int ezdb_append_archive_entries(Ezdb* db, uint32_t archive_id, const EzdbEntryRecord* entries, uint32_t entry_count);
int ezdb_finish_replace_archive_entries(Ezdb* db, uint32_t archive_id);
int ezdb_abort_replace_archive_entries(Ezdb* db, uint32_t archive_id);

/* ===== 元数据键值存储 ===== */

/*
 * 附加键值对元数据，存储在数据库文件同名的 .meta 文件中。
 * 用于保存与数据库关联的任意配置信息。
 */

/* 读取元数据值（调用方需 free 返回的字符串） */
int ezdb_get_meta(Ezdb* db, const char* key, char** out_value);

/* 写入或更新元数据键值对 */
int ezdb_put_meta(Ezdb* db, const char* key, const char* value);

/* ===== 数据库维护 ===== */

/*
 * 压缩（compact）操作：将当前数据（含增量修改）重写为全新的紧凑快照文件。
 * 可回收已删除记录和增量日志占用的空间，重建索引以优化查询性能。
 */
int ezdb_compact(Ezdb* db);

/* 将错误码转换为可读字符串描述 */
const char* ezdb_error_message(int code);

#ifdef __cplusplus
}
#endif
