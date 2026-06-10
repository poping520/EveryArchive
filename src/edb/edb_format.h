#pragma once

#include <stdint.h>
#include <stdio.h>

/* ===== 错误码（与 edb.h 保持一致） ===== */

#ifndef EDB_OK
#define EDB_OK            0
#define EDB_ERR_ARG      -1
#define EDB_ERR_IO       -2
#define EDB_ERR_FORMAT   -3
#define EDB_ERR_MEMORY   -4
#define EDB_ERR_NOT_FOUND -5
#define EDB_ERR_READ_ONLY -6
#endif

/* ===== 文件格式常量 ===== */

#define EDB_MAGIC       "EDB00001"
#define EDB_VERSION     1u
#define EDB_HEADER_SIZE 96
#define EDB_SECTION_DESC_SIZE 48

/* ===== Section IDs ===== */

#define EDB_SEC_ARCHIVE_RECORDS       1u
#define EDB_SEC_ARCHIVE_STRING_POOL   2u
#define EDB_SEC_ENTRY_CORE            3u
#define EDB_SEC_ENTRY_DETAIL_PAGES    4u
#define EDB_SEC_ENTRY_PATH_PAGES      5u
#define EDB_SEC_ENTRY_RAW_BLOBS       6u
#define EDB_SEC_ARCHIVE_POSTINGS      7u
#define EDB_SEC_ARCHIVE_POSTING_INDEX 8u
#define EDB_SEC_ENTRY_POSTINGS        9u
#define EDB_SEC_ENTRY_POSTING_INDEX   10u
#define EDB_SEC_COUNT                 10

/* ===== 压缩常量 ===== */

#define EDB_SECTION_COMPRESSED      1u
#define EDB_COMPRESS_MIN_SIZE       4096u
#define EDB_COMPRESS_MIN_SAVING     256u
#define EDB_COMPRESSION_LEVEL       3

/* ===== 分页常量 ===== */

#define EDB_DETAIL_ENTRIES_PER_PAGE 4096u
#define EDB_PATH_PAGE_SIZE          65536u
#define EDB_RAW_PAGE_SIZE           262144u

/* ===== 磁盘记录 ===== */

typedef struct {
    uint32_t name_offset;
    uint32_t name_len;
    uint64_t file_size;
    uint64_t modified_time;
    char     drive_letter;
    char     reserved[7];
    uint64_t file_ref_number;
    int64_t  usn;
} EdbDiskArchive;

typedef struct {
    uint32_t archive_id;
    uint32_t path_offset;
    uint32_t path_len;
} EdbDiskEntryCore;

typedef struct {
    int64_t  compressed_size;
    uint64_t original_size;
    uint64_t modified_time;
    uint32_t raw_offset;
    uint32_t raw_len;
    uint32_t flags;
    uint32_t reserved;
} EdbDiskEntryDetail;

typedef struct {
    uint32_t key;
    uint32_t count;
    uint32_t container_type;
    uint32_t encoded_size;
    uint32_t raw_size;
    uint32_t reserved;
    uint64_t offset;
} EdbDiskIndex;

typedef struct {
    uint64_t offset;
    uint32_t encoded_size;
    uint32_t raw_size;
    uint32_t flags;
    uint32_t reserved;
} EdbDiskPage;

/* ===== Section 描述符 ===== */

typedef struct {
    uint32_t section_id;
    uint32_t flags;
    uint64_t offset;
    uint64_t encoded_size;
    uint64_t raw_size;
    uint64_t aux_offset;
    uint32_t aux_count;
    uint32_t page_size;
    uint64_t reserved;
} EdbSectionDesc;

/* ===== 文件头 ===== */

typedef struct {
    char     magic[8];
    uint32_t version;
    uint32_t header_size;
    uint32_t flags;
    uint32_t section_count;
    uint64_t archive_count;
    uint64_t active_archive_count;
    uint64_t entry_count;
    uint64_t active_entry_count;
    uint64_t section_table_offset;
    uint32_t checksum;
    char     reserved[28];
} EdbHeader;

/* ===== 内存模型类型 ===== */

#define EDB_CACHE_SLOTS 64

typedef struct {
    uint32_t page_id;
    void*    data;
    uint32_t tick;
    int      valid;
} EdbCacheSlot;

typedef struct {
    EdbCacheSlot slots[EDB_CACHE_SLOTS];
    uint32_t    tick_counter;
} EdbPageCache;

/* ===== 格式读写 API ===== */

int  edb_format_write_header(FILE* fp, const EdbHeader* hdr);
int  edb_format_read_header(FILE* fp, uint64_t file_size, EdbHeader* out);

int  edb_format_write_section_table(FILE* fp, const EdbSectionDesc* sections,
                                     uint32_t count, uint64_t* out_offset);
int  edb_format_read_section_table(FILE* fp, uint64_t table_offset, uint32_t count,
                                    EdbSectionDesc* out);

const EdbSectionDesc* edb_format_find_section(const EdbSectionDesc* sections,
                                               uint32_t count, uint32_t id);

/* ===== 压缩 ===== */

int edb_format_compress(const uint8_t* src, uint32_t src_len,
                         uint8_t** out, uint32_t* out_len);
int edb_format_decompress(const uint8_t* src, uint32_t src_len,
                           uint8_t* out, uint32_t out_cap, uint32_t* out_len);

/* ===== 分页缓存 ===== */

void    edb_page_cache_init(EdbPageCache* cache);
void    edb_page_cache_free(EdbPageCache* cache);
void*   edb_page_cache_get(EdbPageCache* cache, uint32_t page_id);
void    edb_page_cache_put(EdbPageCache* cache, uint32_t page_id, void* data, uint32_t size);

/* ===== 分页读写 ===== */

int edb_format_read_page(FILE* fp, const EdbDiskPage* pages, uint32_t page_idx,
                          EdbPageCache* cache, void** out_data, uint32_t* out_size);
int edb_format_read_entry_detail(FILE* fp, EdbDiskPage* pages, uint32_t page_count,
                                  EdbPageCache* cache, uint32_t entry_id,
                                  EdbDiskEntryDetail* out);
int edb_format_read_entry_path(FILE* fp, EdbDiskPage* pages, uint32_t page_count,
                                EdbPageCache* cache,
                                uint32_t path_offset, uint32_t path_len,
                                char** out_path);
int edb_format_read_entry_raw(FILE* fp, EdbDiskPage* pages, uint32_t page_count,
                               EdbPageCache* cache,
                               uint32_t raw_offset, uint32_t raw_len,
                               void** out_raw);
