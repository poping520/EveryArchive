#pragma once

#include "ezdb.h"
#include "ezdb_internal.h"

#include <stdio.h>
#include <stdint.h>

#define EZDB_ENTRY_PAGE_SIZE 4096u
#define EZDB_RAW_BLOB_PAGE_SIZE (256u * 1024u)
#define EZDB_ENTRY_DETAIL_CACHE_PAGES 8u
#define EZDB_RAW_BLOB_CACHE_PAGES 64u
#define EZDB_ENTRY_CORE_RECORD_SIZE 12u

typedef struct EzdbPageCacheEntry {
    uint32_t page_id;
    uint32_t size;
    uint64_t tick;
    unsigned char* data;
} EzdbPageCacheEntry;

typedef struct EzdbEntrySource {
    void* user_data;
    int (*reset)(void* user_data);
    int (*reset_range)(void* user_data, uint32_t archive_begin, uint32_t archive_end);
    int (*next)(void* user_data, EzdbEntryRecord* out_record);
    int (*open_range)(void* user_data, uint32_t archive_begin, uint32_t archive_end, struct EzdbEntrySource* out_source);
    void (*close_range)(struct EzdbEntrySource* source);
} EzdbEntrySource;

typedef struct EzdbEntryDetailStore {
    FILE* fp;
    EzdbDiskPage* pages;
    uint32_t page_count;
    uint64_t section_offset;
    EzdbPageCacheEntry* cache;
    uint32_t cache_count;
    uint64_t* cache_tick;
    uint32_t entry_count;
    const uint32_t* archive_ids;
    const uint32_t* path_offsets;
    const uint32_t* path_lens;
    const unsigned char* delta_bits;
    const EzdbDeltaEntryRef* delta_refs;
} EzdbEntryDetailStore;

typedef struct EzdbEntryPathStore {
    FILE* fp;
    uint32_t entry_count;
    const uint32_t* path_offsets;
    const uint32_t* path_lens;
    const unsigned char* delta_bits;
    const EzdbDeltaEntryRef* delta_refs;
    EzdbDiskPage* raw_blob_pages;
    uint32_t raw_blob_page_count;
    uint64_t raw_blob_section_offset;
    uint64_t raw_blob_raw_size;
    EzdbPageCacheEntry* raw_blob_cache;
    uint32_t raw_blob_cache_count;
    uint64_t* cache_tick;
} EzdbEntryPathStore;

typedef struct EzdbCompactEntrySource {
    const uint32_t* archive_id_map;
    const unsigned char* active_entry_bits;
    const unsigned char* active_archive_bits;
    uint32_t file_count;
    uint32_t next_entry_id;
    char* entry_path;
    void* raw_path;
    EzdbEntryDetailStore detail_store;
    EzdbEntryPathStore path_store;
} EzdbCompactEntrySource;

void ezdb_entries_page_cache_free(EzdbPageCacheEntry* cache, uint32_t count);
int ezdb_entries_load_page_cached(FILE* fp,
                                  EzdbDiskPage* pages,
                                  uint32_t page_count,
                                  uint64_t section_offset,
                                  uint32_t page_id,
                                  EzdbPageCacheEntry* cache,
                                  uint32_t cache_count,
                                  uint64_t* cache_tick,
                                  const unsigned char** out_data,
                                  uint32_t* out_size);
int ezdb_entries_copy_raw_blob_range(FILE* fp,
                                     EzdbDiskPage* pages,
                                     uint32_t page_count,
                                     uint64_t section_offset,
                                     uint64_t raw_size,
                                     EzdbPageCacheEntry* cache,
                                     uint32_t cache_count,
                                     uint64_t* cache_tick,
                                     uint32_t offset,
                                     uint32_t len,
                                     unsigned char* out);
int ezdb_entries_copy_delta_blob_range(FILE* fp, uint64_t offset, uint32_t len, unsigned char* out);
int ezdb_entries_load_detail(const EzdbEntryDetailStore* store, uint32_t id, EzdbDiskEntry* out);
char* ezdb_entries_copy_path(const EzdbEntryPathStore* store, uint32_t id);
void* ezdb_entries_copy_raw_path(const EzdbEntryPathStore* store, uint32_t id, const EzdbDiskEntry* detail);
void ezdb_entries_compact_source_clear_current(EzdbCompactEntrySource* source);
int ezdb_entries_compact_source_reset(void* user_data);
int ezdb_entries_compact_source_next(void* user_data, EzdbEntryRecord* out_record);
void ezdb_entries_encode_core(const EzdbDiskEntry* entry, unsigned char out[EZDB_ENTRY_CORE_RECORD_SIZE]);
void ezdb_entries_decode_core(const unsigned char raw[EZDB_ENTRY_CORE_RECORD_SIZE], EzdbDiskEntry* out);
