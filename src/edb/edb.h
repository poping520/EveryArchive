#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Edb Edb;

/* ===== 记录类型 ===== */

typedef struct EdbFileRecord {
    char         drive_letter;
    uint64_t     file_ref_number;
    int64_t      usn;
    const char*  file_path;
    uint64_t     file_size;
    uint64_t     modified_time;
} EdbFileRecord;

typedef EdbFileRecord EdbArchiveRecord;

typedef struct EdbEntryRecord {
    uint32_t     archive_id;
    const char*  entry_path;
    const void*  entry_raw_path;
    uint32_t     entry_raw_path_len;
    int64_t      compressed_size;
    uint64_t     original_size;
    uint64_t     modified_time;
} EdbEntryRecord;

/* ===== 流式接口 ===== */

typedef struct EdbEntryStream {
    void* user_data;
    int  (*reset)(void* user_data);
    int  (*reset_range)(void* user_data, uint32_t archive_begin, uint32_t archive_end);
    int  (*next)(void* user_data, EdbEntryRecord* out_record);
    int  (*open_range)(void* user_data, uint32_t archive_begin, uint32_t archive_end,
                       struct EdbEntryStream* out_stream);
    void (*close_range)(struct EdbEntryStream* stream);
} EdbEntryStream;

/* ===== 构建选项 ===== */

#define EDB_BUILD_ENTRY_INDEX         0x01u
#define EDB_BUILD_POSTING_COMPRESSION 0x02u
#define EDB_BUILD_DEFAULT_FLAGS (EDB_BUILD_ENTRY_INDEX | EDB_BUILD_POSTING_COMPRESSION)

typedef struct EdbBuildOptions {
    const char* temp_dir;
    uint32_t    memory_limit_mb;
    uint32_t    flags;
    uint32_t    log_level;
    uint32_t    index_threads;
} EdbBuildOptions;

/* ===== 搜索结果 ===== */

typedef enum EdbSearchKind {
    EDB_RESULT_ARCHIVE = 1,
    EDB_RESULT_ENTRY    = 2
} EdbSearchKind;

typedef struct EdbArchiveResult {
    uint32_t id;
    char     drive_letter;
    uint64_t file_ref_number;
    int64_t  usn;
    char*    file_path;
    uint64_t file_size;
    uint64_t modified_time;
} EdbArchiveResult;

typedef struct EdbEntryResult {
    uint32_t id;
    uint32_t archive_id;
    char*    archive_path;
    char*    entry_path;
    void*    entry_raw_path;
    uint32_t entry_raw_path_len;
    int64_t  compressed_size;
    uint64_t original_size;
    uint64_t modified_time;
} EdbEntryResult;

typedef struct EdbSearchResult {
    EdbSearchKind kind;
    uint32_t id;
    uint32_t archive_id;
    char     drive_letter;
    uint64_t file_ref_number;
    int64_t  usn;
    char*    archive_path;
    char*    entry_path;
    void*    entry_raw_path;
    uint32_t entry_raw_path_len;
    int64_t  compressed_size;
    uint64_t original_size;
    uint64_t file_size;
    uint64_t modified_time;
} EdbSearchResult;

/* ===== 搜索范围 ===== */

#define EDB_SEARCH_ARCHIVE_PATH  0x01u
#define EDB_SEARCH_ENTRY_PATH    0x02u
#define EDB_SEARCH_COMBINED_PATH 0x04u
#define EDB_SEARCH_ALL (EDB_SEARCH_ARCHIVE_PATH | EDB_SEARCH_ENTRY_PATH)

/* ===== 分页查询 ===== */

typedef struct EdbEntryQuery {
    const char* keyword;
    uint32_t    scope;
    int         sort_column;
    int         sort_ascending;
    uint32_t    offset;
    uint32_t    limit;
} EdbEntryQuery;

typedef struct EdbEntryQueryPage {
    uint64_t   total_count;
    uint32_t   returned_count;
    uint32_t*  ids;
} EdbEntryQueryPage;

/* ===== 统计信息 ===== */

typedef struct EdbStats {
    uint32_t archive_count;
    uint32_t active_archive_count;
    uint32_t entry_count;
    uint32_t active_entry_count;
    uint64_t file_size;
    uint64_t archive_records_size;
    uint64_t archive_strings_size;
    uint64_t entry_core_size;
    uint64_t entry_detail_size;
    uint64_t entry_path_size;
    uint64_t raw_blob_size;
    uint64_t archive_index_size;
    uint64_t archive_postings_size;
    uint64_t entry_index_size;
    uint64_t entry_postings_size;
} EdbStats;

/* ===== 回调 ===== */

typedef void (*EdbSearchCallback)(const EdbSearchResult* result, void* user_data);

/* ===== 错误码 ===== */

#define EDB_OK            0
#define EDB_ERR_ARG      -1
#define EDB_ERR_IO       -2
#define EDB_ERR_FORMAT   -3
#define EDB_ERR_MEMORY   -4
#define EDB_ERR_NOT_FOUND -5
#define EDB_ERR_READ_ONLY -6

/* ===== 快照构建 ===== */

int edb_build_snapshot(const EdbArchiveRecord* archives, uint32_t archive_count,
                       const EdbEntryRecord* entries, uint32_t entry_count,
                       const char* output_path);

int edb_build_snapshot_stream(const EdbArchiveRecord* archives, uint32_t archive_count,
                               EdbEntryStream* entry_stream, uint32_t entry_count,
                               const char* output_path, const EdbBuildOptions* options);

/* ===== 数据库开关 ===== */

int  edb_open(const char* path, Edb** out_db);
void edb_close(Edb* db);

/* ===== 计数查询 ===== */

uint32_t edb_count(Edb* db);
uint32_t edb_active_count(Edb* db);
uint32_t edb_entry_count(Edb* db);
uint32_t edb_active_entry_count(Edb* db);
uint64_t edb_file_size(Edb* db);
int      edb_stats(Edb* db, EdbStats* out);

/* ===== 单条记录查询 ===== */

int edb_get_archive(Edb* db, uint32_t id, EdbArchiveResult* out);
int edb_get_entry(Edb* db, uint32_t id, EdbEntryResult* out);
int edb_get_archive_by_ref(Edb* db, char drive_letter, uint64_t file_ref_number,
                            EdbArchiveResult* out);

/* ===== 释放函数 ===== */

void edb_free_archive_result(EdbArchiveResult* result);
void edb_free_entry_result(EdbEntryResult* result);
void edb_free_search_result(EdbSearchResult* result);
void edb_free_query_page(EdbEntryQueryPage* page);

/* ===== 搜索 ===== */

int edb_search(Edb* db, const char* keyword, uint32_t scope, uint32_t limit,
               EdbSearchCallback callback, void* user_data);

int edb_query_entries(Edb* db, const EdbEntryQuery* query, EdbEntryQueryPage* out);

int edb_get_entries_batch(Edb* db, const uint32_t* ids, uint32_t count,
                           EdbEntryResult* out);

/* ===== 写入事务 ===== */

int edb_begin_write(Edb* db);
int edb_commit_write(Edb* db);
int edb_rollback_write(Edb* db);

/* ===== 压缩包 CRUD ===== */

int edb_upsert_archive(Edb* db, const EdbArchiveRecord* record, uint32_t* out_id);

int edb_upsert_archives(Edb* db, const EdbArchiveRecord* records, uint32_t count,
                         uint32_t* out_ids);

int edb_delete_archive_by_ref(Edb* db, char drive_letter, uint64_t file_ref_number);

/* ===== 条目 CRUD ===== */

int edb_replace_archive_entries(Edb* db, uint32_t archive_id,
                                 const EdbEntryRecord* entries, uint32_t entry_count);

int edb_begin_replace_archive_entries(Edb* db, uint32_t archive_id);
int edb_append_archive_entries(Edb* db, uint32_t archive_id,
                                const EdbEntryRecord* entries, uint32_t entry_count);
int edb_finish_replace_archive_entries(Edb* db, uint32_t archive_id);
int edb_abort_replace_archive_entries(Edb* db, uint32_t archive_id);

/* ===== 元数据 ===== */

int edb_get_meta(Edb* db, const char* key, char** out_value);
int edb_put_meta(Edb* db, const char* key, const char* value);

/* ===== 维护 ===== */

int         edb_compact(Edb* db);
const char* edb_error_message(int code);

#ifdef __cplusplus
}
#endif
