#pragma once

#include "ezdb.h"
#include "ezdb_entries.h"
#include "ezdb_format.h"
#include "ezdb_internal.h"
#include "ezdb_postings.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct EzdbDeltaRecord {
    uint32_t id;
    uint32_t type;
    char* path;
    uint32_t path_len;
    uint64_t size;
    uint64_t modified_time;
    uint32_t next_by_id;
} EzdbDeltaRecord;

struct Ezdb {
    FILE* fp;
    char* path;
    int read_only;
    int format_v13;
    uint64_t v13_section_table_offset;
    uint64_t v13_section_table_size;
    EzdbHeader header;
    uint32_t* file_parent_dir_ids;
    uint32_t* file_name_offsets;
    uint16_t* file_name_lens;
    uint32_t* file_sizes32;
    uint32_t* file_size_overflow_ids;
    uint64_t* file_size_overflow_values;
    uint32_t file_size_overflow_count;
    uint32_t file_size_overflow_id_cap;
    uint32_t file_size_overflow_value_cap;
    uint32_t* file_modified_times32;
    uint32_t* file_mtime_overflow_ids;
    uint64_t* file_mtime_overflow_values;
    uint32_t file_mtime_overflow_count;
    uint32_t file_mtime_overflow_id_cap;
    uint32_t file_mtime_overflow_value_cap;
    EzdbDiskArchiveMeta* archive_meta;
    uint32_t* entry_archive_ids;
    uint32_t* entry_path_offsets;
    uint32_t* entry_path_lens;
    uint32_t* entry_next_in_archive;
    uint32_t* archive_first_entry_ids;
    EzdbDeltaEntryRef* delta_entry_refs;
    unsigned char* delta_entry_bits;
    size_t delta_entry_bits_cap_bytes;
    EzdbDiskPage* entry_detail_pages;
    uint64_t entry_arrays_cap;
    EzdbDiskPage* raw_blob_pages;
    EzdbPageCacheEntry entry_detail_cache[EZDB_ENTRY_DETAIL_CACHE_PAGES];
    EzdbPageCacheEntry raw_blob_cache[EZDB_RAW_BLOB_CACHE_PAGES];
    uint64_t cache_tick;
    unsigned char* active_entry_bits;
    size_t active_entry_bits_cap_bytes;
    EzdbDiskDir* dirs;
    char* strings;
    EzdbDiskIndex* file_index;
    EzdbDiskIndex* dir_index;
    EzdbDiskIndex* entry_index;
    PostingBuilder delta_entry_index;
    int delta_entry_index_ready;
    unsigned char* active_bits;
    size_t active_bits_cap_bytes;
    unsigned char* covered_base_bits;
    EzdbDeltaRecord* deltas;
    uint32_t delta_count;
    uint32_t delta_cap;
    uint32_t* delta_buckets;
    uint32_t delta_bucket_count;
    uint32_t write_txn_active;
    EzdbHeader txn_start_header;
    uint32_t txn_start_delta_count;
    uint32_t txn_start_delta_cap;
    unsigned char* txn_start_active_bits;
    size_t txn_start_active_bit_bytes;
    unsigned char* txn_start_active_entry_bits;
    size_t txn_start_active_entry_bit_bytes;
    uint64_t txn_start_entry_count;
    uint64_t txn_start_active_entry_count;
    int batch_index_deferred;
    int batch_index_dirty;

    /* Bulk write mode: buffer archives + entries in memory, build at commit */
    int bulk_write_mode;
    EzdbArchiveRecord* bulk_archives;
    char** bulk_archive_paths;       /* strdup'd file_path for each archive */
    uint32_t bulk_archive_count;
    uint32_t bulk_archive_cap;
    uint32_t* bulk_archive_id_map;   /* original archive_id -> bulk index */
    EzdbEntryRecord* bulk_entries;
    char** bulk_entry_paths;         /* strdup'd entry_path per entry */
    void** bulk_entry_raw_paths;     /* malloc'd raw_path per entry */
    uint32_t bulk_entry_count;
    uint32_t bulk_entry_cap;

    /* Path cache for non-bulk transactions: avoids disk reads in rebuild */
    char** delta_entry_path_cache;   /* entry_id -> strdup'd path */
    uint32_t delta_entry_path_cache_cap;
};

double ezdb_now_ms(void);
char* ezdb_strdup_range(const char* text, size_t len);
uint32_t ezdb_fnv1a_bytes(const char* text, size_t len);
int ezdb_ensure_capacity(void** data, size_t elem_size, uint32_t* capacity, uint32_t needed);
void ezdb_bitset_set(unsigned char* bits, uint32_t id, int value);
int ezdb_bitset_get(const unsigned char* bits, uint32_t id);
int ezdb_bitset_or_into(unsigned char* dst, const unsigned char* src, size_t size);
int ezdb_bitset_and_into(unsigned char* dst, const unsigned char* src, size_t size);
int ezdb_bitset_any(const unsigned char* data, size_t size);
uint32_t ezdb_next_pow2_u32(uint32_t value);

EzdbDeltaRecord* ezdb_find_delta_record(Ezdb* db, uint32_t id);
int ezdb_delta_hash_ensure(Ezdb* db, uint32_t needed_records);
void ezdb_delta_hash_reset(Ezdb* db);
int ezdb_delta_hash_add_latest(Ezdb* db, uint32_t delta_index);
int ezdb_append_delta_memory(Ezdb* db,
                             uint32_t type,
                             uint32_t id,
                             const char* path,
                             uint32_t path_len,
                             uint64_t size,
                             uint64_t modified_time);
int ezdb_delta_entry_index_add_path(Ezdb* db, uint32_t entry_id, const char* path);
int ezdb_delta_entry_index_remove_path(Ezdb* db, uint32_t entry_id, const char* path);
int ezdb_delta_entry_index_ensure(Ezdb* db);
int ezdb_rebuild_delta_entry_index(Ezdb* db);
int ezdb_replay_delta_log(Ezdb* db);

int ezdb_resize_entry_arrays(Ezdb* db, uint64_t entry_count);
int ezdb_resize_active_bits(Ezdb* db, uint64_t file_count);
int ezdb_ensure_active_bits_zero_extended(Ezdb* db, uint64_t old_file_count, uint64_t new_file_count);
void ezdb_link_entry_to_archive(Ezdb* db, uint32_t entry_id, uint32_t archive_id);
void ezdb_rebuild_archive_entry_links(Ezdb* db);
int ezdb_deactivate_entries_for_archive(Ezdb* db, uint32_t archive_id);

EzdbEntryDetailStore ezdb_entry_detail_store(Ezdb* db);
EzdbEntryPathStore ezdb_entry_path_store(Ezdb* db);
int ezdb_entry_is_searchable(Ezdb* db, uint32_t entry_id);
int ezdb_build_result_path(Ezdb* db, uint32_t id, EzdbSearchResult* out_result);
uint64_t ezdb_file_size_by_id(Ezdb* db, uint32_t id);
uint64_t ezdb_file_modified_time_by_id(Ezdb* db, uint32_t id);
const char* ezdb_file_name_by_id(Ezdb* db, uint32_t id);
int ezdb_write_header(Ezdb* db);
uint64_t ezdb_delta_append_offset(Ezdb* db);
void ezdb_release_members(Ezdb* db, int free_path);
