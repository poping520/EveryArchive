#pragma once

#include "edb.h"
#include "edb_format.h"
#include "edb_postings.h"
#include <stdint.h>
#include <stdio.h>

/* ===== 分页写入器 ===== */

typedef struct {
    FILE*         fp;
    uint32_t      page_size;       /* 每页字节数 */
    uint8_t*      buf;             /* 当前页缓冲区 */
    uint32_t      buf_used;        /* 当前页已用字节 */
    EdbDiskPage*  pages;           /* 页索引 */
    uint32_t      page_count;
    uint32_t      page_cap;
} EdbPagedWriter;

int  edb_paged_writer_init(EdbPagedWriter* w, FILE* fp, uint32_t page_size);
void edb_paged_writer_free(EdbPagedWriter* w);
int  edb_paged_writer_append(EdbPagedWriter* w, const void* data, uint32_t len);
int  edb_paged_writer_flush(EdbPagedWriter* w);
uint64_t edb_paged_writer_total_raw(EdbPagedWriter* w);

/* ===== 字符串去重器 ===== */

typedef struct {
    char*    pool;          /* 字符串池 */
    uint32_t pool_size;
    uint32_t pool_cap;
    uint32_t* buckets;      /* 哈希桶 */
    uint32_t  bucket_count;
} EdbStringDedup;

int  edb_string_dedup_init(EdbStringDedup* d, uint32_t bucket_count, uint32_t pool_cap);
void edb_string_dedup_free(EdbStringDedup* d);
int  edb_string_dedup_intern(EdbStringDedup* d, const char* str, uint32_t len,
                              uint32_t* out_offset);

/* ===== 快照构建核心 ===== */

int edb_build_snapshot_impl(const EdbArchiveRecord* archives, uint32_t archive_count,
                             EdbEntryStream* entry_stream, uint32_t entry_count,
                             const char* output_path, const EdbBuildOptions* options);
