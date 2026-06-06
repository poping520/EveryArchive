/*
 * ezdb v7 path-tree index
 * =======================
 *
 * v7 intentionally breaks compatibility with v1/v2/v3/v4/v5/v6. Instead of indexing every
 * byte gram in every full path, it stores a directory tree and builds separate
 * gram indexes for file names and directory components. Directory hits expand
 * to DFS file-id ranges, so a common directory term is stored once per matching
 * directory node rather than repeated for every child file.
 *
 * The on-disk layout is:
 * header, file records, directory records, string pool, file index, directory
 * index, postings, append-only delta log. Postings use adaptive array/range/bitset
 * containers and independently choose zlib compression when it reduces disk size.
 * Inserts, updates and deletes append tiny delta records. Single-record writes still
 * flush immediately, while write transactions batch many delta records and patch the
 * header once at commit so bulk CRUD avoids per-row disk sync.
 */

#include "ezdb.h"
#include "ezdb_entries.h"
#include "ezdb_internal.h"
#include "ezdb_io.h"
#include "ezdb_postings.h"
#include "ezdb_query.h"

#include <ctype.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <windows.h>
#include <psapi.h>

#define EZDB_WRITE_TXN_ACTIVE 1u

typedef struct EzdbDeltaRecord {
    uint32_t id;
    uint32_t type;
    char* path;
    uint32_t path_len;
    uint64_t size;
    uint64_t modified_time;
    uint32_t next_by_id;
} EzdbDeltaRecord;

typedef struct BuildDir {
    uint32_t name_offset;
    uint32_t name_len;
    uint32_t parent;
    uint32_t first_child;
    uint32_t next_sibling;
    uint32_t first_file;
    uint32_t old_first_file;
    uint32_t old_file_count;
    uint32_t first_file_id;
    uint32_t file_count;
} BuildDir;

typedef struct BuildFile {
    uint32_t name_offset;
    uint32_t name_len;
    uint32_t parent_dir;
    uint32_t next_in_dir;
    uint32_t original_id;
    uint64_t size;
    uint64_t modified_time;
    char drive_letter;
    uint64_t file_ref_number;
    int64_t usn;
} BuildFile;

typedef struct DirHashEntry {
    uint32_t parent;
    uint32_t name_offset;
    uint32_t name_len;
    uint32_t dir_id;
    uint32_t next;
} DirHashEntry;

typedef struct StringHashEntry {
    uint32_t offset;
    uint32_t len;
    uint32_t next;
} StringHashEntry;

typedef struct EzdbBuildOptionsResolved {
    char temp_dir[1024];
    uint32_t memory_limit_mb;
    uint32_t flags;
    uint32_t log_level;
    uint32_t index_threads;
    uint32_t zip_threads;
} EzdbBuildOptionsResolved;

static int append_blob(unsigned char** data, uint32_t* size, uint32_t* cap, const void* bytes, uint32_t len, uint32_t extra_nul, uint32_t* out_offset);
static int entry_is_searchable(Ezdb* db, uint32_t entry_id);
static uint64_t ezdb_delta_append_offset(Ezdb* db);
static EzdbEntryDetailStore entry_detail_store(Ezdb* db);
static EzdbEntryPathStore entry_path_store(Ezdb* db);
static int delta_entry_index_remove_path(Ezdb* db, uint32_t entry_id, const char* path);

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
    uint64_t entry_arrays_cap; /* preallocated capacity for entry arrays */
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
    int batch_index_deferred; /* skip per-entry gram index during bulk import */
    int batch_index_dirty;    /* true if delta entries need gram index rebuild */
};

static uint64_t file_size_of(FILE* fp)
{
    long old_pos = ftell(fp);
    if (fseek(fp, 0, SEEK_END) != 0) return 0;
    long size = ftell(fp);
    fseek(fp, old_pos, SEEK_SET);
    return size < 0 ? 0 : (uint64_t)size;
}

static double ezdb_now_ms(void)
{
    return (double)clock() * 1000.0 / (double)CLOCKS_PER_SEC;
}

static double ezdb_peak_working_set_mb(void)
{
    PROCESS_MEMORY_COUNTERS_EX counters;
    memset(&counters, 0, sizeof(counters));
    counters.cb = sizeof(counters);
    if (!GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&counters, sizeof(counters))) return 0.0;
    return (double)counters.PeakWorkingSetSize / 1024.0 / 1024.0;
}

static char* ezdb_strdup_range(const char* text, size_t len)
{
    char* out = (char*)malloc(len + 1);
    if (!out) return NULL;
    memcpy(out, text, len);
    out[len] = '\0';
    return out;
}

static int ensure_capacity(void** data, size_t elem_size, uint32_t* capacity, uint32_t needed)
{
    if (*capacity >= needed) return EZDB_OK;
    uint32_t next = *capacity ? *capacity : 1024;
    while (next < needed) {
        if (next > UINT32_MAX / 2u) return EZDB_ERR_MEMORY;
        next *= 2u;
    }
    void* new_data = realloc(*data, elem_size * (size_t)next);
    if (!new_data) return EZDB_ERR_MEMORY;
    *data = new_data;
    *capacity = next;
    return EZDB_OK;
}

static uint32_t fnv1a_bytes(const char* text, size_t len)
{
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < len; ++i) {
        hash ^= (unsigned char)text[i];
        hash *= 16777619u;
    }
    return hash;
}

static int append_string(char** data, uint32_t* size, uint32_t* cap, const char* text, uint32_t len, uint32_t* out_offset)
{
    if (ensure_capacity((void**)data, 1, cap, *size + len + 1u) != EZDB_OK) return EZDB_ERR_MEMORY;
    *out_offset = *size;
    memcpy(*data + *size, text, len);
    (*data)[*size + len] = '\0';
    *size += len + 1u;
    return EZDB_OK;
}

static int init_u32_buckets(uint32_t** buckets, uint32_t count)
{
    *buckets = (uint32_t*)malloc(sizeof(uint32_t) * count);
    if (!*buckets) return EZDB_ERR_MEMORY;
    for (uint32_t i = 0; i < count; ++i) (*buckets)[i] = UINT32_MAX;
    return EZDB_OK;
}

static int find_or_add_string(const char* text,
                              uint32_t len,
                              char** pool,
                              uint32_t* pool_size,
                              uint32_t* pool_cap,
                              StringHashEntry** entries,
                              uint32_t* entry_count,
                              uint32_t* entry_cap,
                              uint32_t** buckets,
                              uint32_t* bucket_count,
                              uint32_t* out_offset)
{
    if (!*buckets) {
        *bucket_count = 65536u;
        if (init_u32_buckets(buckets, *bucket_count) != EZDB_OK) return EZDB_ERR_MEMORY;
    }
    uint32_t hash = fnv1a_bytes(text, len);
    uint32_t bucket = hash & (*bucket_count - 1u);
    for (uint32_t i = (*buckets)[bucket]; i != UINT32_MAX; i = (*entries)[i].next) {
        if ((*entries)[i].len == len && memcmp(*pool + (*entries)[i].offset, text, len) == 0) {
            *out_offset = (*entries)[i].offset;
            return EZDB_OK;
        }
    }

    uint32_t offset = 0;
    if (append_string(pool, pool_size, pool_cap, text, len, &offset) != EZDB_OK) return EZDB_ERR_MEMORY;
    if (ensure_capacity((void**)entries, sizeof(StringHashEntry), entry_cap, *entry_count + 1) != EZDB_OK) return EZDB_ERR_MEMORY;
    StringHashEntry* e = &(*entries)[*entry_count];
    e->offset = offset;
    e->len = len;
    e->next = (*buckets)[bucket];
    (*buckets)[bucket] = *entry_count;
    *entry_count += 1;
    *out_offset = offset;
    return EZDB_OK;
}

static uint32_t find_or_add_dir(BuildDir** dirs,
                                uint32_t* dir_count,
                                uint32_t* dir_cap,
                                DirHashEntry** hash_entries,
                                uint32_t* hash_count,
                                uint32_t* hash_cap,
                                uint32_t** buckets,
                                uint32_t* bucket_count,
                                char** string_pool,
                                uint32_t* string_size,
                                uint32_t* string_cap,
                                StringHashEntry** string_entries,
                                uint32_t* string_entry_count,
                                uint32_t* string_entry_cap,
                                uint32_t** string_buckets,
                                uint32_t* string_bucket_count,
                                uint32_t parent,
                                const char* name,
                                uint32_t name_len)
{
    if (!*buckets) {
        *bucket_count = 262144u;
        if (init_u32_buckets(buckets, *bucket_count) != EZDB_OK) return UINT32_MAX;
    }
    uint32_t hash = fnv1a_bytes(name, name_len) ^ (parent * 16777619u);
    uint32_t bucket = hash & (*bucket_count - 1u);
    for (uint32_t i = (*buckets)[bucket]; i != UINT32_MAX; i = (*hash_entries)[i].next) {
        DirHashEntry* he = &(*hash_entries)[i];
        if (he->parent == parent && he->name_len == name_len &&
            memcmp(*string_pool + he->name_offset, name, name_len) == 0) {
            return he->dir_id;
        }
    }

    if (ensure_capacity((void**)dirs, sizeof(BuildDir), dir_cap, *dir_count + 1) != EZDB_OK) return UINT32_MAX;
    uint32_t name_offset = 0;
    if (find_or_add_string(name, name_len, string_pool, string_size, string_cap,
                           string_entries, string_entry_count, string_entry_cap,
                           string_buckets, string_bucket_count, &name_offset) != EZDB_OK) {
        return UINT32_MAX;
    }

    uint32_t id = *dir_count;
    BuildDir* dir = &(*dirs)[id];
    memset(dir, 0, sizeof(*dir));
    dir->name_offset = name_offset;
    dir->name_len = name_len;
    dir->parent = parent;
    dir->first_child = UINT32_MAX;
    dir->next_sibling = UINT32_MAX;
    dir->first_file = UINT32_MAX;
    dir->old_first_file = UINT32_MAX;
    if (id != parent && parent != UINT32_MAX) {
        dir->next_sibling = (*dirs)[parent].first_child;
        (*dirs)[parent].first_child = id;
    }
    *dir_count += 1;

    if (ensure_capacity((void**)hash_entries, sizeof(DirHashEntry), hash_cap, *hash_count + 1) != EZDB_OK) return UINT32_MAX;
    DirHashEntry* he = &(*hash_entries)[*hash_count];
    he->parent = parent;
    he->name_offset = name_offset;
    he->name_len = name_len;
    he->dir_id = id;
    he->next = (*buckets)[bucket];
    (*buckets)[bucket] = *hash_count;
    *hash_count += 1;
    return id;
}

static uint32_t get_or_create_path_dir(BuildDir** dirs,
                                       uint32_t* dir_count,
                                       uint32_t* dir_cap,
                                       DirHashEntry** hash_entries,
                                       uint32_t* hash_count,
                                       uint32_t* hash_cap,
                                       uint32_t** buckets,
                                       uint32_t* bucket_count,
                                       char** string_pool,
                                       uint32_t* string_size,
                                       uint32_t* string_cap,
                                       StringHashEntry** string_entries,
                                       uint32_t* string_entry_count,
                                       uint32_t* string_entry_cap,
                                       uint32_t** string_buckets,
                                       uint32_t* string_bucket_count,
                                       const char* path,
                                       uint32_t path_len)
{
    uint32_t parent = 0;
    uint32_t start = 0;
    for (uint32_t i = 0; i <= path_len; ++i) {
        if (i == path_len || path[i] == '\\' || path[i] == '/') {
            if (i > start) {
                parent = find_or_add_dir(dirs, dir_count, dir_cap, hash_entries, hash_count, hash_cap,
                                         buckets, bucket_count, string_pool, string_size, string_cap,
                                         string_entries, string_entry_count, string_entry_cap,
                                         string_buckets, string_bucket_count, parent, path + start, i - start);
                if (parent == UINT32_MAX) return UINT32_MAX;
            }
            start = i + 1;
        }
    }
    return parent;
}

static int append_file(BuildFile** files,
                       uint32_t* file_count,
                       uint32_t* file_cap,
                       BuildDir* dirs,
                       uint32_t dir_id,
                       char** string_pool,
                       uint32_t* string_size,
                       uint32_t* string_cap,
                       StringHashEntry** string_entries,
                       uint32_t* string_entry_count,
                       uint32_t* string_entry_cap,
                       uint32_t** string_buckets,
                       uint32_t* string_bucket_count,
                       const char* name,
                       uint32_t name_len,
                       uint32_t original_id,
                       uint64_t size,
                       uint64_t modified_time,
                       char drive_letter,
                       uint64_t file_ref_number,
                       int64_t usn)
{
    if (ensure_capacity((void**)files, sizeof(BuildFile), file_cap, *file_count + 1) != EZDB_OK) return EZDB_ERR_MEMORY;
    uint32_t name_offset = 0;
    if (find_or_add_string(name, name_len, string_pool, string_size, string_cap,
                           string_entries, string_entry_count, string_entry_cap,
                           string_buckets, string_bucket_count, &name_offset) != EZDB_OK) {
        return EZDB_ERR_MEMORY;
    }
    uint32_t id = *file_count;
    BuildFile* f = &(*files)[id];
    memset(f, 0, sizeof(*f));
    f->name_offset = name_offset;
    f->name_len = name_len;
    f->parent_dir = dir_id;
    f->original_id = original_id;
    f->size = size;
    f->modified_time = modified_time;
    f->drive_letter = drive_letter;
    f->file_ref_number = file_ref_number;
    f->usn = usn;
    f->next_in_dir = dirs[dir_id].old_first_file;
    dirs[dir_id].old_first_file = id;
    dirs[dir_id].old_file_count += 1;
    *file_count += 1;
    return EZDB_OK;
}

static int u32_compare(const void* a, const void* b)
{
    uint32_t av = *(const uint32_t*)a;
    uint32_t bv = *(const uint32_t*)b;
    if (av == bv) return 0;
    return av < bv ? -1 : 1;
}

static int index_compare(const void* a, const void* b)
{
    const EzdbDiskIndex* ia = (const EzdbDiskIndex*)a;
    const EzdbDiskIndex* ib = (const EzdbDiskIndex*)b;
    if (ia->key == ib->key) return 0;
    return ia->key < ib->key ? -1 : 1;
}

static uint32_t next_pow2_u32(uint32_t value)
{
    uint32_t out = 1;
    while (out < value && out < 0x80000000u) out <<= 1u;
    return out ? out : 1u;
}

static uint32_t delta_bucket_for(uint32_t id, uint32_t bucket_count)
{
    uint32_t x = id;
    x ^= x >> 16u;
    x *= 0x7feb352du;
    x ^= x >> 15u;
    return x & (bucket_count - 1u);
}

static void delta_hash_reset(Ezdb* db)
{
    if (!db->delta_buckets || !db->delta_bucket_count) return;
    for (uint32_t i = 0; i < db->delta_bucket_count; ++i) db->delta_buckets[i] = UINT32_MAX;
    for (uint32_t i = 0; i < db->delta_count; ++i) {
        uint32_t bucket = delta_bucket_for(db->deltas[i].id, db->delta_bucket_count);
        db->deltas[i].next_by_id = db->delta_buckets[bucket];
        db->delta_buckets[bucket] = i;
    }
}

static int delta_hash_ensure(Ezdb* db, uint32_t needed_records)
{
    uint32_t wanted = next_pow2_u32(needed_records * 2u + 16u);
    if (wanted < 16u) wanted = 16u;
    if (db->delta_bucket_count >= wanted) return EZDB_OK;
    uint32_t* buckets = (uint32_t*)malloc(sizeof(uint32_t) * (size_t)wanted);
    if (!buckets) return EZDB_ERR_MEMORY;
    free(db->delta_buckets);
    db->delta_buckets = buckets;
    db->delta_bucket_count = wanted;
    delta_hash_reset(db);
    return EZDB_OK;
}

static EzdbDeltaRecord* find_delta_record(Ezdb* db, uint32_t id)
{
    if (!db || !db->delta_buckets || !db->delta_bucket_count) return NULL;
    uint32_t bucket = delta_bucket_for(id, db->delta_bucket_count);
    for (uint32_t i = db->delta_buckets[bucket]; i != UINT32_MAX; i = db->deltas[i].next_by_id) {
        if (db->deltas[i].id == id) return &db->deltas[i];
    }
    return NULL;
}

static int delta_hash_add_latest(Ezdb* db, uint32_t delta_index)
{
    if (!db || delta_index >= db->delta_count) return EZDB_ERR_ARG;
    if (delta_hash_ensure(db, db->delta_count) != EZDB_OK) return EZDB_ERR_MEMORY;
    EzdbDeltaRecord* rec = &db->deltas[delta_index];
    uint32_t bucket = delta_bucket_for(rec->id, db->delta_bucket_count);
    uint32_t* link = &db->delta_buckets[bucket];
    while (*link != UINT32_MAX) {
        EzdbDeltaRecord* cur = &db->deltas[*link];
        if (cur->id == rec->id) {
            rec->next_by_id = cur->next_by_id;
            *link = delta_index;
            return EZDB_OK;
        }
        link = &cur->next_by_id;
    }
    rec->next_by_id = UINT32_MAX;
    *link = delta_index;
    return EZDB_OK;
}

static int append_delta_memory(Ezdb* db, uint32_t type, uint32_t id, const char* path, uint32_t path_len, uint64_t size, uint64_t modified_time)
{
    if (delta_hash_ensure(db, db->delta_count + 1u) != EZDB_OK) return EZDB_ERR_MEMORY;

    EzdbDeltaRecord* existing = db->write_txn_active ? NULL : find_delta_record(db, id);
    if (existing) {
        free(existing->path);
        existing->type = type;
        existing->size = size;
        existing->modified_time = modified_time;
        existing->path_len = path_len;
        existing->path = NULL;
        if (path_len) {
            existing->path = ezdb_strdup_range(path, path_len);
            if (!existing->path) return EZDB_ERR_MEMORY;
        }
        return EZDB_OK;
    }

    if (ensure_capacity((void**)&db->deltas, sizeof(EzdbDeltaRecord), &db->delta_cap, db->delta_count + 1u) != EZDB_OK) {
        return EZDB_ERR_MEMORY;
    }
    EzdbDeltaRecord* rec = &db->deltas[db->delta_count];
    memset(rec, 0, sizeof(*rec));
    rec->id = id;
    rec->type = type;
    rec->size = size;
    rec->modified_time = modified_time;
    rec->path_len = path_len;
    rec->next_by_id = UINT32_MAX;
    if (path_len) {
        rec->path = ezdb_strdup_range(path, path_len);
        if (!rec->path) return EZDB_ERR_MEMORY;
    }
    db->delta_count += 1u;
    int rc = delta_hash_add_latest(db, db->delta_count - 1u);
    if (rc != EZDB_OK) {
        free(rec->path);
        db->delta_count -= 1u;
        delta_hash_reset(db);
    }
    return rc;
}

static void bitset_set(unsigned char* bits, uint32_t id, int value)
{
    unsigned char mask = (unsigned char)(1u << (id & 7u));
    if (value) bits[id >> 3u] |= mask;
    else bits[id >> 3u] &= (unsigned char)~mask;
}

static int bitset_get(const unsigned char* bits, uint32_t id)
{
    return bits && (bits[id >> 3u] & (unsigned char)(1u << (id & 7u)));
}

static int resize_entry_arrays(Ezdb* db, uint64_t entry_count)
{
    if (!db || entry_count > UINT32_MAX) return EZDB_ERR_ARG;
    if (entry_count <= db->entry_arrays_cap) return EZDB_OK;
    uint64_t next = db->entry_arrays_cap ? db->entry_arrays_cap : 1024u;
    while (next < entry_count) {
        if (next > UINT32_MAX / 2u) { next = UINT32_MAX; break; }
        next *= 2u;
    }
    size_t count = (size_t)(next ? next : 1u);
    uint32_t* archive_ids = (uint32_t*)realloc(db->entry_archive_ids, sizeof(uint32_t) * count);
    if (!archive_ids) return EZDB_ERR_MEMORY;
    db->entry_archive_ids = archive_ids;
    uint32_t* path_offsets = (uint32_t*)realloc(db->entry_path_offsets, sizeof(uint32_t) * count);
    if (!path_offsets) return EZDB_ERR_MEMORY;
    db->entry_path_offsets = path_offsets;
    uint32_t* path_lens = (uint32_t*)realloc(db->entry_path_lens, sizeof(uint32_t) * count);
    if (!path_lens) return EZDB_ERR_MEMORY;
    db->entry_path_lens = path_lens;
    uint32_t* entry_next = (uint32_t*)realloc(db->entry_next_in_archive, sizeof(uint32_t) * count);
    if (!entry_next) return EZDB_ERR_MEMORY;
    db->entry_next_in_archive = entry_next;
    EzdbDeltaEntryRef* refs = (EzdbDeltaEntryRef*)realloc(db->delta_entry_refs, sizeof(EzdbDeltaEntryRef) * count);
    if (!refs) return EZDB_ERR_MEMORY;
    db->delta_entry_refs = refs;

    size_t bit_bytes = ((size_t)next + 7u) / 8u;
    unsigned char* bits = (unsigned char*)realloc(db->active_entry_bits, bit_bytes ? bit_bytes : 1u);
    if (!bits) return EZDB_ERR_MEMORY;
    db->active_entry_bits = bits;
    db->active_entry_bits_cap_bytes = bit_bytes ? bit_bytes : 1u;
    unsigned char* delta_bits = (unsigned char*)realloc(db->delta_entry_bits, bit_bytes ? bit_bytes : 1u);
    if (!delta_bits) return EZDB_ERR_MEMORY;
    db->delta_entry_bits = delta_bits;
    db->delta_entry_bits_cap_bytes = bit_bytes ? bit_bytes : 1u;
    db->entry_arrays_cap = next;
    return EZDB_OK;
}

static int ensure_entry_arrays_zero_extended(Ezdb* db, uint64_t old_count, uint64_t new_count)
{
    size_t old_bit_bytes = ((size_t)old_count + 7u) / 8u;
    int rc = resize_entry_arrays(db, new_count);
    if (rc != EZDB_OK) return rc;
    if (new_count > old_count) {
        size_t add = (size_t)(new_count - old_count);
        memset(db->entry_archive_ids + old_count, 0, sizeof(uint32_t) * add);
        memset(db->entry_path_offsets + old_count, 0, sizeof(uint32_t) * add);
        memset(db->entry_path_lens + old_count, 0, sizeof(uint32_t) * add);
        for (uint64_t i = old_count; i < new_count; ++i) db->entry_next_in_archive[i] = UINT32_MAX;
        memset(db->delta_entry_refs + old_count, 0, sizeof(EzdbDeltaEntryRef) * add);
        size_t new_bit_bytes = ((size_t)new_count + 7u) / 8u;
        if (new_bit_bytes > old_bit_bytes) {
            memset(db->active_entry_bits + old_bit_bytes, 0, new_bit_bytes - old_bit_bytes);
            memset(db->delta_entry_bits + old_bit_bytes, 0, new_bit_bytes - old_bit_bytes);
        }
    }
    if (old_count && (old_count & 7u)) {
        db->active_entry_bits[old_count >> 3u] &= (unsigned char)((1u << (old_count & 7u)) - 1u);
        db->delta_entry_bits[old_count >> 3u] &= (unsigned char)((1u << (old_count & 7u)) - 1u);
    }
    return EZDB_OK;
}

static int resize_active_bits(Ezdb* db, uint64_t file_count)
{
    size_t bit_bytes = ((size_t)file_count + 7u) / 8u;
    uint64_t old_file_count = db->header.file_count;
    size_t archive_count = (size_t)(file_count ? file_count : 1u);
    uint32_t* archive_first = (uint32_t*)realloc(db->archive_first_entry_ids, sizeof(uint32_t) * archive_count);
    if (!archive_first) return EZDB_ERR_MEMORY;
    db->archive_first_entry_ids = archive_first;
    for (uint64_t i = old_file_count; i < file_count; ++i) db->archive_first_entry_ids[i] = UINT32_MAX;
    if (db->active_bits_cap_bytes >= bit_bytes) return EZDB_OK;
    size_t wanted = db->active_bits_cap_bytes ? db->active_bits_cap_bytes : 1024u;
    while (wanted < bit_bytes) wanted *= 2u;
    unsigned char* new_active = (unsigned char*)realloc(db->active_bits, wanted ? wanted : 1u);
    if (!new_active) return EZDB_ERR_MEMORY;
    if (wanted > db->active_bits_cap_bytes) memset(new_active + db->active_bits_cap_bytes, 0, wanted - db->active_bits_cap_bytes);
    db->active_bits = new_active;
    db->active_bits_cap_bytes = wanted;
    return EZDB_OK;
}

static void clear_archive_entry_links(Ezdb* db)
{
    if (!db || !db->archive_first_entry_ids || !db->entry_next_in_archive) return;
    for (uint64_t i = 0; i < db->header.file_count; ++i) db->archive_first_entry_ids[i] = UINT32_MAX;
    for (uint64_t i = 0; i < db->header.entry_count; ++i) db->entry_next_in_archive[i] = UINT32_MAX;
}

static void link_entry_to_archive(Ezdb* db, uint32_t entry_id, uint32_t archive_id)
{
    if (!db || !db->archive_first_entry_ids || !db->entry_next_in_archive) return;
    if (entry_id >= db->header.entry_count || archive_id >= db->header.file_count) return;
    db->entry_next_in_archive[entry_id] = db->archive_first_entry_ids[archive_id];
    db->archive_first_entry_ids[archive_id] = entry_id;
}

static void rebuild_archive_entry_links(Ezdb* db)
{
    clear_archive_entry_links(db);
    if (!db || !db->active_entry_bits) return;
    for (uint32_t e = 0; e < db->header.entry_count; ++e) {
        uint32_t archive_id = db->entry_archive_ids[e];
        if (archive_id < db->header.file_count && bitset_get(db->active_entry_bits, e)) {
            link_entry_to_archive(db, e, archive_id);
        }
    }
}

static int deactivate_entries_for_archive(Ezdb* db, uint32_t archive_id)
{
    if (!db || archive_id >= db->header.file_count) return EZDB_ERR_NOT_FOUND;
    if (!db->archive_first_entry_ids || !db->entry_next_in_archive) return EZDB_OK;
    EzdbEntryPathStore path_store = entry_path_store(db);
    uint32_t e = db->archive_first_entry_ids[archive_id];
    db->archive_first_entry_ids[archive_id] = UINT32_MAX;
    while (e != UINT32_MAX && e < db->header.entry_count) {
        uint32_t next = db->entry_next_in_archive[e];
        db->entry_next_in_archive[e] = UINT32_MAX;
        if (db->entry_archive_ids[e] == archive_id && bitset_get(db->active_entry_bits, e)) {
            if (bitset_get(db->delta_entry_bits, e)) {
                char* entry_path = ezdb_entries_copy_path(&path_store, e);
                if (entry_path) {
                    int rc = delta_entry_index_remove_path(db, e, entry_path);
                    free(entry_path);
                    if (rc != EZDB_OK) return rc;
                }
            }
            bitset_set(db->active_entry_bits, e, 0);
            if (db->header.active_entry_count) db->header.active_entry_count -= 1u;
        }
        e = next;
    }
    return EZDB_OK;
}

static int ensure_active_bits_zero_extended(Ezdb* db, uint64_t old_file_count, uint64_t new_file_count)
{
    size_t old_bytes = ((size_t)old_file_count + 7u) / 8u;
    size_t new_bytes = ((size_t)new_file_count + 7u) / 8u;
    int rc = resize_active_bits(db, new_file_count);
    if (rc != EZDB_OK) return rc;
    if (new_bytes > old_bytes) memset(db->active_bits + old_bytes, 0, new_bytes - old_bytes);
    if (old_file_count && (old_file_count & 7u)) {
        db->active_bits[old_file_count >> 3u] &=
            (unsigned char)((1u << (old_file_count & 7u)) - 1u);
    }
    return EZDB_OK;
}

static int flush_file(FILE* fp)
{
    if (fflush(fp) != 0) return EZDB_ERR_IO;
    if (_commit(_fileno(fp)) != 0) return EZDB_ERR_IO;
    return EZDB_OK;
}

static int delta_entry_index_ensure(Ezdb* db)
{
    if (!db) return EZDB_ERR_ARG;
    if (db->delta_entry_index_ready) return EZDB_OK;
    int rc = ezdb_postings_builder_init(&db->delta_entry_index, 4096u);
    if (rc != EZDB_OK) return rc;
    db->delta_entry_index_ready = 1;
    return EZDB_OK;
}

static int delta_entry_index_add_path(Ezdb* db, uint32_t entry_id, const char* path)
{
    if (!path || !*path) return EZDB_OK;
    int rc = delta_entry_index_ensure(db);
    if (rc != EZDB_OK) return rc;
    return ezdb_postings_add_text_grams(&db->delta_entry_index, path, entry_id, 0);
}

static int delta_entry_index_remove_path(Ezdb* db, uint32_t entry_id, const char* path)
{
    if (!db || !path || !*path || !db->delta_entry_index_ready) return EZDB_OK;
    uint32_t* keys = NULL;
    uint32_t key_count = 0;
    int rc = ezdb_postings_build_query_keys(path, &keys, &key_count);
    if (rc != EZDB_OK) {
        free(keys);
        return rc;
    }
    uint32_t seen_stack[EZDB_STACK_KEYS];
    uint32_t* seen = key_count <= EZDB_STACK_KEYS ? seen_stack : (uint32_t*)malloc(sizeof(uint32_t) * (size_t)key_count);
    if (!seen) {
        free(keys);
        return EZDB_ERR_MEMORY;
    }
    uint32_t seen_count = 0;
    for (uint32_t i = 0; i < key_count; ++i) {
        int duplicate = 0;
        for (uint32_t j = 0; j < seen_count; ++j) {
            if (seen[j] == keys[i]) {
                duplicate = 1;
                break;
            }
        }
        if (!duplicate) {
            seen[seen_count++] = keys[i];
            rc = ezdb_postings_builder_remove_id(&db->delta_entry_index, keys[i], entry_id);
            if (rc != EZDB_OK) break;
        }
    }
    if (seen != seen_stack) free(seen);
    free(keys);
    return rc;
}

static int rebuild_delta_entry_index(Ezdb* db)
{
    if (!db) return EZDB_ERR_ARG;
    ezdb_postings_builder_free(&db->delta_entry_index);
    memset(&db->delta_entry_index, 0, sizeof(db->delta_entry_index));
    db->delta_entry_index_ready = 0;
    EzdbEntryPathStore path_store = entry_path_store(db);
    for (uint32_t e = 0; e < db->header.entry_count; ++e) {
        if (!bitset_get(db->active_entry_bits, e) || !bitset_get(db->delta_entry_bits, e)) continue;
        char* entry_path = ezdb_entries_copy_path(&path_store, e);
        if (!entry_path) continue;
        int rc = delta_entry_index_add_path(db, e, entry_path);
        free(entry_path);
        if (rc != EZDB_OK) return rc;
    }
    return EZDB_OK;
}

static int append_varuint(unsigned char** data, uint32_t* size, uint32_t* cap, uint32_t value)
{
    unsigned char bytes[5];
    uint32_t count = 0;
    do {
        bytes[count] = (unsigned char)(value & 0x7fu);
        value >>= 7u;
        if (value) bytes[count] |= 0x80u;
        ++count;
    } while (value);
    if (*size + count > *cap) {
        uint32_t next = *cap ? *cap : 256u;
        while (next < *size + count) {
            if (next > UINT32_MAX / 2u) return EZDB_ERR_MEMORY;
            next *= 2u;
        }
        unsigned char* new_data = (unsigned char*)realloc(*data, next);
        if (!new_data) return EZDB_ERR_MEMORY;
        *data = new_data;
        *cap = next;
    }
    memcpy(*data + *size, bytes, count);
    *size += count;
    return EZDB_OK;
}

static int append_varuint64(unsigned char** data, uint32_t* size, uint32_t* cap, uint64_t value)
{
    unsigned char bytes[10];
    uint32_t count = 0;
    do {
        bytes[count] = (unsigned char)(value & 0x7fu);
        value >>= 7u;
        if (value) bytes[count] |= 0x80u;
        ++count;
    } while (value);
    if (*size + count > *cap) {
        uint32_t next = *cap ? *cap : 256u;
        while (next < *size + count) {
            if (next > UINT32_MAX / 2u) return EZDB_ERR_MEMORY;
            next *= 2u;
        }
        unsigned char* new_data = (unsigned char*)realloc(*data, next);
        if (!new_data) return EZDB_ERR_MEMORY;
        *data = new_data;
        *cap = next;
    }
    memcpy(*data + *size, bytes, count);
    *size += count;
    return EZDB_OK;
}

static int encode_file_records_compact(const BuildFile* files, uint32_t file_count, unsigned char** out_data, uint64_t* out_size)
{
    unsigned char* data = NULL;
    uint32_t size = 0, cap = 0;
    for (uint32_t i = 0; i < file_count; ++i) {
        int rc = append_varuint(&data, &size, &cap, files[i].parent_dir);
        if (rc == EZDB_OK) rc = append_varuint(&data, &size, &cap, files[i].name_offset);
        if (rc == EZDB_OK) rc = append_varuint(&data, &size, &cap, files[i].name_len);
        if (rc == EZDB_OK) rc = append_varuint64(&data, &size, &cap, files[i].size);
        if (rc == EZDB_OK) rc = append_varuint64(&data, &size, &cap, files[i].modified_time);
        if (rc != EZDB_OK) {
            free(data);
            return rc;
        }
    }
    *out_data = data;
    *out_size = size;
    return EZDB_OK;
}

static int store_file_record(Ezdb* db, uint32_t id, uint32_t parent_dir, uint32_t name_offset, uint32_t name_len, uint64_t size, uint64_t modified_time)
{
    if (name_len > UINT16_MAX) return EZDB_ERR_FORMAT;
    db->file_parent_dir_ids[id] = parent_dir;
    db->file_name_offsets[id] = name_offset;
    db->file_name_lens[id] = (uint16_t)name_len;
    if (size <= UINT32_MAX) {
        db->file_sizes32[id] = (uint32_t)size;
    } else {
        if (ensure_capacity((void**)&db->file_size_overflow_ids, sizeof(uint32_t), &db->file_size_overflow_id_cap, db->file_size_overflow_count + 1) != EZDB_OK ||
            ensure_capacity((void**)&db->file_size_overflow_values, sizeof(uint64_t), &db->file_size_overflow_value_cap, db->file_size_overflow_count + 1) != EZDB_OK) {
            return EZDB_ERR_MEMORY;
        }
        db->file_sizes32[id] = UINT32_MAX;
        db->file_size_overflow_ids[db->file_size_overflow_count] = id;
        db->file_size_overflow_values[db->file_size_overflow_count] = size;
        ++db->file_size_overflow_count;
    }
    if (modified_time <= UINT32_MAX) {
        db->file_modified_times32[id] = (uint32_t)modified_time;
    } else {
        if (ensure_capacity((void**)&db->file_mtime_overflow_ids, sizeof(uint32_t), &db->file_mtime_overflow_id_cap, db->file_mtime_overflow_count + 1) != EZDB_OK ||
            ensure_capacity((void**)&db->file_mtime_overflow_values, sizeof(uint64_t), &db->file_mtime_overflow_value_cap, db->file_mtime_overflow_count + 1) != EZDB_OK) {
            return EZDB_ERR_MEMORY;
        }
        db->file_modified_times32[id] = UINT32_MAX;
        db->file_mtime_overflow_ids[db->file_mtime_overflow_count] = id;
        db->file_mtime_overflow_values[db->file_mtime_overflow_count] = modified_time;
        ++db->file_mtime_overflow_count;
    }
    return EZDB_OK;
}

static int read_file_records_compact_stream(FILE* fp, uint64_t offset, uint64_t encoded_size, uint32_t flags, Ezdb* db, uint32_t file_count)
{
    SectionVarReader reader;
    int rc = section_var_reader_init(&reader, fp, offset, encoded_size, flags);
    if (rc != EZDB_OK) return rc;
    for (uint32_t i = 0; i < file_count; ++i) {
        uint32_t parent_dir = 0, name_offset = 0, name_len = 0;
        uint64_t size = 0, modified_time = 0;
        rc = section_var_reader_varuint(&reader, &parent_dir);
        if (rc == EZDB_OK) rc = section_var_reader_varuint(&reader, &name_offset);
        if (rc == EZDB_OK) rc = section_var_reader_varuint(&reader, &name_len);
        if (rc == EZDB_OK) rc = section_var_reader_varuint64(&reader, &size);
        if (rc == EZDB_OK) rc = section_var_reader_varuint64(&reader, &modified_time);
        if (rc == EZDB_OK) rc = store_file_record(db, i, parent_dir, name_offset, name_len, size, modified_time);
        if (rc != EZDB_OK) break;
    }
    section_var_reader_close(&reader);
    return rc;
}

static int resolve_build_options(const char* output_ezdb, const EzdbBuildOptions* options, EzdbBuildOptionsResolved* out)
{
    if (!output_ezdb || !out) return EZDB_ERR_ARG;
    memset(out, 0, sizeof(*out));
    out->memory_limit_mb = options && options->memory_limit_mb ? options->memory_limit_mb : 512u;
    if (out->memory_limit_mb < 32u) out->memory_limit_mb = 32u;
    out->flags = options && options->flags ? options->flags : EZDB_BUILD_DEFAULT_FLAGS;
    out->log_level = options ? options->log_level : 0u;
    out->index_threads = options && options->index_threads ? options->index_threads : 1u;
    if (out->index_threads > 64u) out->index_threads = 64u;
    out->zip_threads = options && options->zip_threads ? options->zip_threads : 0u;
    if (options && options->temp_dir && options->temp_dir[0]) {
        if (snprintf(out->temp_dir, sizeof(out->temp_dir), "%s", options->temp_dir) >= (int)sizeof(out->temp_dir)) return EZDB_ERR_ARG;
    } else {
        if (snprintf(out->temp_dir, sizeof(out->temp_dir), "%s.tmp", output_ezdb) >= (int)sizeof(out->temp_dir)) return EZDB_ERR_ARG;
    }
    return EZDB_OK;
}

static int decode_entry_core(Ezdb* db, const unsigned char* raw, uint64_t raw_size)
{
    if (!db) return EZDB_ERR_ARG;
    uint64_t expected = db->header.entry_count * (uint64_t)EZDB_ENTRY_CORE_RECORD_SIZE;
    if (raw_size != expected) return EZDB_ERR_FORMAT;
    if (!db->header.entry_count) return EZDB_OK;
    for (uint32_t i = 0; i < db->header.entry_count; ++i) {
        const unsigned char* p = raw + (uint64_t)i * EZDB_ENTRY_CORE_RECORD_SIZE;
        EzdbDiskEntry core;
        ezdb_entries_decode_core(p, &core);
        if (core.archive_id >= db->header.file_count ||
            (uint64_t)core.entry_path_offset + core.entry_path_len > db->header.raw_blob_raw_size) {
            return EZDB_ERR_FORMAT;
        }
        db->entry_archive_ids[i] = core.archive_id;
        db->entry_path_offsets[i] = core.entry_path_offset;
        db->entry_path_lens[i] = core.entry_path_len;
    }
    return EZDB_OK;
}

static EzdbEntryDetailStore entry_detail_store(Ezdb* db)
{
    EzdbEntryDetailStore store;
    memset(&store, 0, sizeof(store));
    if (!db) return store;
    store.fp = db->fp;
    store.pages = db->entry_detail_pages;
    store.page_count = (uint32_t)db->header.entry_detail_page_count;
    store.section_offset = db->header.entry_detail_offset;
    store.cache = db->entry_detail_cache;
    store.cache_count = EZDB_ENTRY_DETAIL_CACHE_PAGES;
    store.cache_tick = &db->cache_tick;
    store.entry_count = (uint32_t)db->header.entry_count;
    store.archive_ids = db->entry_archive_ids;
    store.path_offsets = db->entry_path_offsets;
    store.path_lens = db->entry_path_lens;
    store.delta_bits = db->delta_entry_bits;
    store.delta_refs = db->delta_entry_refs;
    return store;
}

static EzdbEntryPathStore entry_path_store(Ezdb* db)
{
    EzdbEntryPathStore store;
    memset(&store, 0, sizeof(store));
    if (!db) return store;
    store.fp = db->fp;
    store.entry_count = (uint32_t)db->header.entry_count;
    store.path_offsets = db->entry_path_offsets;
    store.path_lens = db->entry_path_lens;
    store.delta_bits = db->delta_entry_bits;
    store.delta_refs = db->delta_entry_refs;
    store.raw_blob_pages = db->raw_blob_pages;
    store.raw_blob_page_count = (uint32_t)db->header.raw_blob_page_count;
    store.raw_blob_section_offset = db->header.raw_blob_offset;
    store.raw_blob_raw_size = db->header.raw_blob_raw_size;
    store.raw_blob_cache = db->raw_blob_cache;
    store.raw_blob_cache_count = EZDB_RAW_BLOB_CACHE_PAGES;
    store.cache_tick = &db->cache_tick;
    return store;
}

static int replay_delta_log(Ezdb* db)
{
    if (!db->header.delta_offset || !db->header.delta_size) return EZDB_OK;
    if (fseek(db->fp, (long)db->header.delta_offset, SEEK_SET) != 0) return EZDB_ERR_IO;
    uint64_t remaining = db->header.delta_size;
    while (remaining) {
        if (remaining < sizeof(EzdbDeltaDiskHeader)) return EZDB_ERR_FORMAT;
        uint64_t frame_offset = (uint64_t)ftell(db->fp);
        uint32_t magic = 0, type = 0;
        if (fread(&magic, sizeof(magic), 1, db->fp) != 1 ||
            fread(&type, sizeof(type), 1, db->fp) != 1) return EZDB_ERR_IO;
        if (fseek(db->fp, (long)frame_offset, SEEK_SET) != 0) return EZDB_ERR_IO;
        if (magic != EZDB_DELTA_MAGIC) return EZDB_ERR_FORMAT;
        if (type == EZDB_DELTA_ENTRY_APPEND) {
            if (remaining < sizeof(EzdbEntryDeltaDiskHeader)) return EZDB_ERR_FORMAT;
            EzdbEntryDeltaDiskHeader eh;
            if (fread(&eh, sizeof(eh), 1, db->fp) != 1) return EZDB_ERR_IO;
            remaining -= sizeof(eh);
            if (eh.archive_id >= db->header.file_count ||
                eh.entry_path_len > (64u * 1024u * 1024u) ||
                eh.entry_raw_path_len > (64u * 1024u * 1024u) ||
                remaining < (uint64_t)eh.entry_path_len + eh.entry_raw_path_len) {
                return EZDB_ERR_FORMAT;
            }
            if (eh.id >= db->header.entry_count) return EZDB_ERR_FORMAT;
            uint64_t path_offset = (uint64_t)ftell(db->fp);
            uint64_t raw_offset = path_offset + eh.entry_path_len;
            if (fseek(db->fp, (long)(eh.entry_path_len + eh.entry_raw_path_len), SEEK_CUR) != 0) return EZDB_ERR_IO;
            remaining -= (uint64_t)eh.entry_path_len + eh.entry_raw_path_len;
            db->entry_archive_ids[eh.id] = eh.archive_id;
            db->entry_path_offsets[eh.id] = eh.id;
            db->entry_path_lens[eh.id] = eh.entry_path_len;
            db->delta_entry_refs[eh.id].path_offset = path_offset;
            db->delta_entry_refs[eh.id].path_len = eh.entry_path_len;
            db->delta_entry_refs[eh.id].raw_offset = raw_offset;
            db->delta_entry_refs[eh.id].raw_len = eh.entry_raw_path_len;
            db->delta_entry_refs[eh.id].compressed_size = eh.compressed_size;
            db->delta_entry_refs[eh.id].original_size = eh.original_size;
            db->delta_entry_refs[eh.id].modified_time = eh.modified_time;
            if (!bitset_get(db->active_entry_bits, eh.id)) db->header.active_entry_count += 1u;
            bitset_set(db->active_entry_bits, eh.id, 1);
            bitset_set(db->delta_entry_bits, eh.id, 1);
            link_entry_to_archive(db, eh.id, eh.archive_id);
            EzdbEntryPathStore path_store = entry_path_store(db);
            char* entry_path = ezdb_entries_copy_path(&path_store, eh.id);
            if (entry_path) {
                int rc = delta_entry_index_add_path(db, eh.id, entry_path);
                free(entry_path);
                if (rc != EZDB_OK) return rc;
            }
            continue;
        }
        EzdbDeltaDiskHeader dh;
        if (fread(&dh, sizeof(dh), 1, db->fp) != 1) return EZDB_ERR_IO;
        remaining -= sizeof(dh);
        if (dh.magic != EZDB_DELTA_MAGIC ||
            (dh.type != EZDB_DELTA_INSERT && dh.type != EZDB_DELTA_UPDATE && dh.type != EZDB_DELTA_DELETE &&
             dh.type != EZDB_DELTA_BATCH_BEGIN && dh.type != EZDB_DELTA_BATCH_COMMIT &&
             dh.type != EZDB_DELTA_ENTRY_DELETE_ARCHIVE) ||
            dh.id >= db->header.file_count || dh.path_len > (64u * 1024u * 1024u) ||
            remaining < dh.path_len) {
            return EZDB_ERR_FORMAT;
        }
        if (dh.type == EZDB_DELTA_BATCH_BEGIN || dh.type == EZDB_DELTA_BATCH_COMMIT) {
            if (dh.path_len || dh.id || dh.size || dh.modified_time) return EZDB_ERR_FORMAT;
            continue;
        }
        if (dh.type == EZDB_DELTA_ENTRY_DELETE_ARCHIVE) {
            if (dh.path_len || dh.size || dh.modified_time) return EZDB_ERR_FORMAT;
            int rc = deactivate_entries_for_archive(db, dh.id);
            if (rc != EZDB_OK) return rc;
            continue;
        }
        char* path = NULL;
        if (dh.path_len) {
            path = (char*)malloc((size_t)dh.path_len + 1u);
            if (!path) return EZDB_ERR_MEMORY;
            if (fread(path, 1, dh.path_len, db->fp) != dh.path_len) {
                free(path);
                return EZDB_ERR_IO;
            }
            path[dh.path_len] = '\0';
        }
        remaining -= dh.path_len;

        int rc = append_delta_memory(db, dh.type, dh.id, path ? path : "", dh.path_len, dh.size, dh.modified_time);
        free(path);
        if (rc != EZDB_OK) return rc;

        if (dh.id < db->header.base_file_count) bitset_set(db->covered_base_bits, dh.id, 1);
        if (dh.type == EZDB_DELTA_DELETE) {
            bitset_set(db->active_bits, dh.id, 0);
        } else {
            bitset_set(db->active_bits, dh.id, 1);
        }
    }
    return EZDB_OK;
}

static void truncate_delta_memory(Ezdb* db, uint32_t delta_count)
{
    if (!db || !db->deltas || delta_count >= db->delta_count) {
        if (db) db->delta_count = delta_count < db->delta_count ? delta_count : db->delta_count;
        return;
    }
    for (uint32_t i = delta_count; i < db->delta_count; ++i) free(db->deltas[i].path);
    db->delta_count = delta_count;
    delta_hash_reset(db);
}

static int restore_txn_snapshot(Ezdb* db)
{
    if (!db || !db->write_txn_active) return EZDB_ERR_ARG;
    truncate_delta_memory(db, db->txn_start_delta_count);
    db->header = db->txn_start_header;
    unsigned char* restored = (unsigned char*)realloc(db->active_bits, db->txn_start_active_bit_bytes ? db->txn_start_active_bit_bytes : 1u);
    if (!restored) return EZDB_ERR_MEMORY;
    db->active_bits = restored;
    db->active_bits_cap_bytes = db->txn_start_active_bit_bytes ? db->txn_start_active_bit_bytes : 1u;
    memcpy(db->active_bits, db->txn_start_active_bits, db->txn_start_active_bit_bytes ? db->txn_start_active_bit_bytes : 1u);
    int rc = resize_entry_arrays(db, db->txn_start_entry_count);
    if (rc != EZDB_OK) return rc;
    db->header.entry_count = db->txn_start_entry_count;
    db->header.active_entry_count = db->txn_start_active_entry_count;
    memcpy(db->active_entry_bits, db->txn_start_active_entry_bits, db->txn_start_active_entry_bit_bytes ? db->txn_start_active_entry_bit_bytes : 1u);
    size_t entry_bit_bytes = ((size_t)db->header.entry_count + 7u) / 8u;
    if (db->active_entry_bits_cap_bytes > entry_bit_bytes) {
        memset(db->active_entry_bits + entry_bit_bytes, 0, db->active_entry_bits_cap_bytes - entry_bit_bytes);
        memset(db->delta_entry_bits + entry_bit_bytes, 0, db->delta_entry_bits_cap_bytes - entry_bit_bytes);
    }
    rebuild_archive_entry_links(db);
    return rebuild_delta_entry_index(db);
}

static int write_header(Ezdb* db)
{
    if (db && db->format_v13) return ezdb_write_v13_db_header(db);
    if (fseek(db->fp, 0, SEEK_SET) != 0 || fwrite(&db->header, sizeof(db->header), 1, db->fp) != 1) {
        return EZDB_ERR_IO;
    }
    return flush_file(db->fp);
}

static int append_delta_disk(Ezdb* db, uint32_t type, uint32_t id, const EzdbFileRecord* record, int flush_now)
{
    if (!db || db->read_only || !db->fp) return EZDB_ERR_READ_ONLY;
    uint32_t path_len = 0;
    if (record && record->path) {
        size_t len = strlen(record->path);
        if (len > UINT32_MAX) return EZDB_ERR_ARG;
        path_len = (uint32_t)len;
    }
    if ((type == EZDB_DELTA_INSERT || type == EZDB_DELTA_UPDATE) && (!record || !record->path || !path_len)) {
        return EZDB_ERR_ARG;
    }

    uint64_t append_offset = ezdb_delta_append_offset(db);
    if (fseek(db->fp, (long)append_offset, SEEK_SET) != 0) return EZDB_ERR_IO;
    EzdbDeltaDiskHeader dh;
    memset(&dh, 0, sizeof(dh));
    dh.magic = EZDB_DELTA_MAGIC;
    dh.type = type;
    dh.id = id;
    dh.path_len = path_len;
    dh.size = record ? record->size : 0;
    dh.modified_time = record ? record->modified_time : 0;
    if (fwrite(&dh, sizeof(dh), 1, db->fp) != 1) return EZDB_ERR_IO;
    if (path_len && fwrite(record->path, 1, path_len, db->fp) != path_len) return EZDB_ERR_IO;

    if (!db->header.delta_offset) db->header.delta_offset = append_offset;
    db->header.delta_size += sizeof(dh) + path_len;
    db->header.reserved_offset = db->header.delta_offset + db->header.delta_size;
    db->header.reserved_size = 0;
    if (!flush_now) return EZDB_OK;
    return write_header(db);
}

static int append_delta_frame(Ezdb* db, uint32_t type)
{
    if (!db || db->read_only || !db->fp) return EZDB_ERR_READ_ONLY;
    if (type != EZDB_DELTA_BATCH_BEGIN && type != EZDB_DELTA_BATCH_COMMIT) return EZDB_ERR_ARG;
    uint64_t append_offset = ezdb_delta_append_offset(db);
    if (fseek(db->fp, (long)append_offset, SEEK_SET) != 0) return EZDB_ERR_IO;
    EzdbDeltaDiskHeader dh;
    memset(&dh, 0, sizeof(dh));
    dh.magic = EZDB_DELTA_MAGIC;
    dh.type = type;
    if (fwrite(&dh, sizeof(dh), 1, db->fp) != 1) return EZDB_ERR_IO;
    if (!db->header.delta_offset) db->header.delta_offset = append_offset;
    db->header.delta_size += sizeof(dh);
    db->header.reserved_offset = db->header.delta_offset + db->header.delta_size;
    db->header.reserved_size = 0;
    return EZDB_OK;
}

static int append_entry_delete_archive_frame(Ezdb* db, uint32_t archive_id, int flush_now)
{
    if (!db || db->read_only || !db->fp) return EZDB_ERR_READ_ONLY;
    if (archive_id >= db->header.file_count || !bitset_get(db->active_bits, archive_id)) return EZDB_ERR_NOT_FOUND;
    uint64_t append_offset = ezdb_delta_append_offset(db);
    if (fseek(db->fp, (long)append_offset, SEEK_SET) != 0) return EZDB_ERR_IO;
    EzdbDeltaDiskHeader dh;
    memset(&dh, 0, sizeof(dh));
    dh.magic = EZDB_DELTA_MAGIC;
    dh.type = EZDB_DELTA_ENTRY_DELETE_ARCHIVE;
    dh.id = archive_id;
    if (fwrite(&dh, sizeof(dh), 1, db->fp) != 1) return EZDB_ERR_IO;
    if (!db->header.delta_offset) db->header.delta_offset = append_offset;
    db->header.delta_size += sizeof(dh);
    db->header.reserved_offset = db->header.delta_offset + db->header.delta_size;
    db->header.reserved_size = 0;
    int rc = deactivate_entries_for_archive(db, archive_id);
    if (rc != EZDB_OK) return rc;
    if (!flush_now) return EZDB_OK;
    return write_header(db);
}

static int append_entry_delta_disk(Ezdb* db, uint32_t archive_id, const EzdbEntryRecord* record, uint32_t* out_id, int flush_now)
{
    if (!db || db->read_only || !db->fp || !record || !record->entry_path || !out_id) return EZDB_ERR_ARG;
    if (archive_id >= db->header.file_count || !bitset_get(db->active_bits, archive_id)) return EZDB_ERR_NOT_FOUND;
    size_t path_len_sz = strlen(record->entry_path);
    if (path_len_sz > UINT32_MAX) return EZDB_ERR_ARG;
    uint32_t path_len = (uint32_t)path_len_sz;
    uint32_t raw_len = record->entry_raw_path ? record->entry_raw_path_len : 0;
    uint32_t id = (uint32_t)db->header.entry_count;
    uint64_t old_entry_count = db->header.entry_count;
    uint64_t old_active_entry_count = db->header.active_entry_count;
    uint64_t old_delta_offset = db->header.delta_offset;
    uint64_t old_delta_size = db->header.delta_size;
    uint64_t old_reserved_offset = db->header.reserved_offset;
    db->header.entry_count += 1u;
    db->header.active_entry_count += 1u;
    int rc = ensure_entry_arrays_zero_extended(db, old_entry_count, db->header.entry_count);
    if (rc != EZDB_OK) {
        db->header.entry_count = old_entry_count;
        db->header.active_entry_count = old_active_entry_count;
        return rc;
    }

    uint64_t append_offset = ezdb_delta_append_offset(db);
    if (fseek(db->fp, (long)append_offset, SEEK_SET) != 0) rc = EZDB_ERR_IO;
    EzdbEntryDeltaDiskHeader eh;
    memset(&eh, 0, sizeof(eh));
    eh.magic = EZDB_DELTA_MAGIC;
    eh.type = EZDB_DELTA_ENTRY_APPEND;
    eh.id = id;
    eh.archive_id = archive_id;
    eh.entry_path_len = path_len;
    eh.entry_raw_path_len = raw_len;
    eh.compressed_size = record->compressed_size;
    eh.original_size = record->original_size;
    eh.modified_time = record->modified_time;
    if (rc == EZDB_OK && fwrite(&eh, sizeof(eh), 1, db->fp) != 1) rc = EZDB_ERR_IO;
    if (rc == EZDB_OK && path_len && fwrite(record->entry_path, 1, path_len, db->fp) != path_len) rc = EZDB_ERR_IO;
    if (rc == EZDB_OK && raw_len && fwrite(record->entry_raw_path, 1, raw_len, db->fp) != raw_len) rc = EZDB_ERR_IO;
    if (rc == EZDB_OK) {
        if (!db->header.delta_offset) db->header.delta_offset = append_offset;
        db->header.delta_size += sizeof(eh) + path_len + raw_len;
        db->header.reserved_offset = db->header.delta_offset + db->header.delta_size;
        db->header.reserved_size = 0;
        db->entry_archive_ids[id] = archive_id;
        db->entry_path_offsets[id] = id;
        db->entry_path_lens[id] = path_len;
        db->delta_entry_refs[id].path_offset = append_offset + sizeof(eh);
        db->delta_entry_refs[id].path_len = path_len;
        db->delta_entry_refs[id].raw_offset = append_offset + sizeof(eh) + path_len;
        db->delta_entry_refs[id].raw_len = raw_len;
        db->delta_entry_refs[id].compressed_size = record->compressed_size;
        db->delta_entry_refs[id].original_size = record->original_size;
        db->delta_entry_refs[id].modified_time = record->modified_time;
        bitset_set(db->active_entry_bits, id, 1);
        bitset_set(db->delta_entry_bits, id, 1);
        link_entry_to_archive(db, id, archive_id);
        rc = delta_entry_index_add_path(db, id, record->entry_path);
        if (rc != EZDB_OK) return rc;
        *out_id = id;
        return flush_now ? write_header(db) : EZDB_OK;
    }

    db->header.entry_count = old_entry_count;
    db->header.active_entry_count = old_active_entry_count;
    db->header.delta_offset = old_delta_offset;
    db->header.delta_size = old_delta_size;
    db->header.reserved_offset = old_reserved_offset;
    return rc;
}

static uint32_t dfs_assign(BuildDir* dirs,
                           BuildFile* old_files,
                           BuildFile* new_files,
                           uint32_t dir_id,
                           uint32_t next_file,
                           uint32_t* original_to_final)
{
    BuildDir* dir = &dirs[dir_id];
    dir->first_file_id = next_file;
    for (uint32_t old = dir->old_first_file; old != UINT32_MAX; old = old_files[old].next_in_dir) {
        new_files[next_file] = old_files[old];
        new_files[next_file].parent_dir = dir_id;
        if (original_to_final) original_to_final[old_files[old].original_id] = next_file;
        ++next_file;
    }
    for (uint32_t child = dir->first_child; child != UINT32_MAX; child = dirs[child].next_sibling) {
        next_file = dfs_assign(dirs, old_files, new_files, child, next_file, original_to_final);
    }
    dir->file_count = next_file - dir->first_file_id;
    return next_file;
}

static const char* dir_name(Ezdb* db, const EzdbDiskDir* d)
{
    return db->strings + d->name_offset;
}

static const char* file_name_by_id(Ezdb* db, uint32_t id)
{
    return db->strings + db->file_name_offsets[id];
}

static uint64_t lookup_u64_overflow(uint32_t id, uint32_t inline_value, const uint32_t* ids, const uint64_t* values, uint32_t count)
{
    if (inline_value != UINT32_MAX) return inline_value;
    uint32_t lo = 0;
    uint32_t hi = count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        if (ids[mid] < id) lo = mid + 1u;
        else hi = mid;
    }
    if (lo < count && ids[lo] == id) return values[lo];
    return inline_value;
}

static uint64_t file_size_by_id(Ezdb* db, uint32_t id)
{
    return lookup_u64_overflow(id,
                               db->file_sizes32[id],
                               db->file_size_overflow_ids,
                               db->file_size_overflow_values,
                               db->file_size_overflow_count);
}

static uint64_t file_modified_time_by_id(Ezdb* db, uint32_t id)
{
    return lookup_u64_overflow(id,
                               db->file_modified_times32[id],
                               db->file_mtime_overflow_ids,
                               db->file_mtime_overflow_values,
                               db->file_mtime_overflow_count);
}

static int build_dir_path(Ezdb* db, uint32_t dir_id, char** out, uint32_t* out_len)
{
    uint32_t stack_cap = 32;
    uint32_t stack_count = 0;
    uint32_t* stack = (uint32_t*)malloc(sizeof(uint32_t) * stack_cap);
    if (!stack) return EZDB_ERR_MEMORY;
    uint32_t cur = dir_id;
    while (cur != 0 && cur < db->header.dir_count) {
        if (ensure_capacity((void**)&stack, sizeof(uint32_t), &stack_cap, stack_count + 1) != EZDB_OK) {
            free(stack);
            return EZDB_ERR_MEMORY;
        }
        stack[stack_count++] = cur;
        cur = db->dirs[cur].parent_dir_id;
    }
    uint32_t len = 0;
    for (uint32_t i = 0; i < stack_count; ++i) {
        const EzdbDiskDir* d = &db->dirs[stack[i]];
        len += d->name_len;
        if (i + 1 < stack_count) ++len;
    }
    char* path = (char*)malloc((size_t)len + 1u);
    if (!path) {
        free(stack);
        return EZDB_ERR_MEMORY;
    }
    uint32_t pos = 0;
    for (uint32_t ri = stack_count; ri > 0; --ri) {
        uint32_t id = stack[ri - 1];
        const EzdbDiskDir* d = &db->dirs[id];
        if (pos) path[pos++] = '\\';
        memcpy(path + pos, dir_name(db, d), d->name_len);
        pos += d->name_len;
    }
    path[pos] = '\0';
    free(stack);
    *out = path;
    if (out_len) *out_len = len;
    return EZDB_OK;
}

static int build_result_path(Ezdb* db, uint32_t id, EzdbSearchResult* out_result)
{
    if (!db || !out_result) return EZDB_ERR_ARG;
    if (id >= db->header.file_count) return EZDB_ERR_NOT_FOUND;
    if (!bitset_get(db->active_bits, id)) return EZDB_ERR_NOT_FOUND;
    EzdbDeltaRecord* delta = find_delta_record(db, id);
    if (delta) {
        if (delta->type == EZDB_DELTA_DELETE) return EZDB_ERR_NOT_FOUND;
        memset(out_result, 0, sizeof(*out_result));
        out_result->path = ezdb_strdup_range(delta->path, delta->path_len);
        if (!out_result->path) return EZDB_ERR_MEMORY;
        out_result->id = id;
        out_result->size = delta->size;
        out_result->modified_time = delta->modified_time;
        return EZDB_OK;
    }
    if (id >= db->header.base_file_count) return EZDB_ERR_NOT_FOUND;
    memset(out_result, 0, sizeof(*out_result));
    char* dir_path = NULL;
    uint32_t dir_len = 0;
    uint32_t name_len = db->file_name_lens[id];
    const char* name = file_name_by_id(db, id);
    int rc = build_dir_path(db, db->file_parent_dir_ids[id], &dir_path, &dir_len);
    if (rc != EZDB_OK) return rc;
    uint32_t path_len = dir_len + (dir_len ? 1u : 0u) + name_len;
    char* path = (char*)malloc((size_t)path_len + 1u);
    if (!path) {
        free(dir_path);
        return EZDB_ERR_MEMORY;
    }
    if (dir_len) {
        memcpy(path, dir_path, dir_len);
        path[dir_len] = '\\';
        memcpy(path + dir_len + 1u, name, name_len);
    } else {
        memcpy(path, name, name_len);
    }
    path[path_len] = '\0';
    free(dir_path);
    out_result->id = id;
    out_result->path = path;
    out_result->size = file_size_by_id(db, id);
    out_result->modified_time = file_modified_time_by_id(db, id);
    return EZDB_OK;
}

static int record_contains_keyword(Ezdb* db, uint32_t id, const char* keyword, size_t key_len)
{
    EzdbDeltaRecord* delta = find_delta_record(db, id);
    if (delta) {
        return delta->type != EZDB_DELTA_DELETE &&
               ezdb_query_contains_ascii_casefold(delta->path, delta->path_len, keyword, key_len);
    }
    if (id >= db->header.base_file_count) return 0;
    if (ezdb_query_contains_ascii_casefold(file_name_by_id(db, id), db->file_name_lens[id], keyword, key_len)) return 1;
    char* path = NULL;
    EzdbSearchResult result;
    if (build_result_path(db, id, &result) != EZDB_OK) return 0;
    path = result.path;
    int matched = ezdb_query_contains_ascii_casefold(path, strlen(path), keyword, key_len);
    ezdb_free_result(&result);
    return matched;
}

static int mark_literal_candidates(Ezdb* db, const char* literal, size_t literal_len, unsigned char* seen, int* any_marked)
{
    char* keyword = ezdb_strdup_range(literal, literal_len);
    if (!keyword) return EZDB_ERR_MEMORY;
    uint32_t* keys = NULL;
    uint32_t key_count = 0;
    int rc = ezdb_query_build_candidate_keys(keyword, literal_len, &keys, &key_count);
    if (rc != EZDB_OK) {
        free(keyword);
        return rc;
    }
    if (!key_count) {
        free(keys);
        free(keyword);
        return EZDB_OK;
    }

    uint32_t* file_ids = NULL;
    uint32_t file_count = 0;
    uint32_t* dir_ids = NULL;
    uint32_t dir_count = 0;
    rc = ezdb_postings_load_intersected(db->fp, db->header.postings_offset, db->file_index, db->header.file_index_count, keys, key_count, &file_ids, &file_count);
    if (rc == EZDB_OK) rc = ezdb_postings_load_intersected(db->fp, db->header.postings_offset, db->dir_index, db->header.dir_index_count, keys, key_count, &dir_ids, &dir_count);
    free(keys);
    if (rc != EZDB_OK) {
        free(file_ids);
        free(dir_ids);
        free(keyword);
        return rc;
    }

    for (uint32_t i = 0; i < file_count; ++i) {
        if (file_ids[i] < db->header.base_file_count &&
            bitset_get(db->active_bits, file_ids[i]) &&
            !bitset_get(db->covered_base_bits, file_ids[i])) {
            seen[file_ids[i] >> 3u] |= (unsigned char)(1u << (file_ids[i] & 7u));
            *any_marked = 1;
        }
    }
    for (uint32_t i = 0; i < dir_count; ++i) {
        uint32_t dir_id = dir_ids[i];
        if (dir_id >= db->header.dir_count) continue;
        EzdbDiskDir* d = &db->dirs[dir_id];
        uint32_t end = d->first_file_id + d->file_count;
        if (end > db->header.base_file_count) end = (uint32_t)db->header.base_file_count;
        for (uint32_t id = d->first_file_id; id < end; ++id) {
            if (bitset_get(db->active_bits, id) && !bitset_get(db->covered_base_bits, id)) {
                seen[id >> 3u] |= (unsigned char)(1u << (id & 7u));
                *any_marked = 1;
            }
        }
    }
    for (uint32_t i = 0; i < db->delta_count; ++i) {
        EzdbDeltaRecord* delta = &db->deltas[i];
        if (find_delta_record(db, delta->id) != delta) continue;
        if (delta->type == EZDB_DELTA_DELETE || !bitset_get(db->active_bits, delta->id)) continue;
        if (ezdb_query_contains_ascii_casefold(delta->path, delta->path_len, keyword, literal_len)) {
            seen[delta->id >> 3u] |= (unsigned char)(1u << (delta->id & 7u));
            *any_marked = 1;
        }
    }

    free(file_ids);
    free(dir_ids);
    free(keyword);
    return EZDB_OK;
}

static int bitset_or_into(unsigned char* dst, const unsigned char* src, size_t size)
{
    for (size_t i = 0; i < size; ++i) dst[i] |= src[i];
    return EZDB_OK;
}

static int bitset_and_into(unsigned char* dst, const unsigned char* src, size_t size)
{
    for (size_t i = 0; i < size; ++i) dst[i] &= src[i];
    return EZDB_OK;
}

static int bitset_any(const unsigned char* data, size_t size)
{
    for (size_t i = 0; i < size; ++i) {
        if (data[i]) return 1;
    }
    return 0;
}

static int query_build_candidate_bitset(Ezdb* db, EzdbQueryNode* node, unsigned char** out_bits, int* out_has_positive)
{
    *out_bits = NULL;
    *out_has_positive = 0;
    if (!node) return EZDB_OK;
    size_t bit_bytes = ((size_t)db->header.file_count + 7u) / 8u;
    if (node->type == EZDB_QUERY_NOT) return EZDB_OK;
    if (node->type == EZDB_QUERY_TERM || node->type == EZDB_QUERY_WILDCARD) {
        const char* literal = NULL;
        size_t literal_len = 0;
        if (node->type == EZDB_QUERY_TERM) {
            literal = node->text;
            literal_len = node->text_len;
        } else if (!ezdb_query_longest_literal_from_wildcard(node->text, node->text_len, &literal, &literal_len)) {
            return EZDB_OK;
        }
        unsigned char* bits = (unsigned char*)calloc(bit_bytes ? bit_bytes : 1u, 1);
        if (!bits) return EZDB_ERR_MEMORY;
        int any_marked = 0;
        int rc = mark_literal_candidates(db, literal, literal_len, bits, &any_marked);
        if (rc != EZDB_OK) {
            free(bits);
            return rc;
        }
        *out_bits = bits;
        *out_has_positive = 1;
        return EZDB_OK;
    }
    if (node->type == EZDB_QUERY_AND || node->type == EZDB_QUERY_OR) {
        unsigned char* left = NULL;
        unsigned char* right = NULL;
        int left_positive = 0;
        int right_positive = 0;
        int rc = query_build_candidate_bitset(db, node->left, &left, &left_positive);
        if (rc == EZDB_OK) rc = query_build_candidate_bitset(db, node->right, &right, &right_positive);
        if (rc != EZDB_OK) {
            free(left);
            free(right);
            return rc;
        }
        if (node->type == EZDB_QUERY_AND) {
            if (left_positive && right_positive) {
                bitset_and_into(left, right, bit_bytes);
                free(right);
                *out_bits = left;
                *out_has_positive = 1;
            } else if (left_positive) {
                *out_bits = left;
                *out_has_positive = 1;
                free(right);
            } else if (right_positive) {
                *out_bits = right;
                *out_has_positive = 1;
                free(left);
            } else {
                free(left);
                free(right);
            }
        } else {
            if (left_positive && right_positive) {
                bitset_or_into(left, right, bit_bytes);
                free(right);
                *out_bits = left;
                *out_has_positive = 1;
            } else {
                free(left);
                free(right);
                *out_bits = NULL;
                *out_has_positive = 0;
            }
        }
    }
    return EZDB_OK;
}

static int ezdb_write_entries_from_source(FILE* out,
                                          EzdbHeader* header,
                                          EzdbEntrySource* source,
                                          uint32_t entry_count,
                                          uint32_t original_archive_count,
                                          const uint32_t* original_to_final,
                                          const EzdbBuildOptionsResolved* options)
{
    if (!out || !header || (!source && entry_count)) return EZDB_ERR_ARG;
    int rc = EZDB_OK;
    EzdbEntryCollectResult entry_collect;
    EzdbEntryFinalizeStats entry_finalize_stats;
    PostingBuilder entry_builder;
    int entry_builder_ready = 0;
    EzdbDiskIndex* entry_index = NULL;
    EzdbEntryIndexBuildStats entry_index_stats;
    double total_start_ms = ezdb_now_ms();
    double stream_total_ms = 0.0;
    double index_count_ms = 0.0;
    double index_reduce_ms = 0.0;
    double index_prepare_ms = 0.0;
    double index_fill_ms = 0.0;
    double postings_build_ms = 0.0;
    double write_postings_ms = 0.0;
    int parallel_count_possible = 0;

    if (!options) return EZDB_ERR_ARG;
    memset(&entry_collect, 0, sizeof(entry_collect));
    memset(&entry_finalize_stats, 0, sizeof(entry_finalize_stats));
    memset(&entry_builder, 0, sizeof(entry_builder));
    memset(&entry_index_stats, 0, sizeof(entry_index_stats));
    if (rc == EZDB_OK && entry_count && (options->flags & EZDB_BUILD_ENTRY_INDEX)) {
        rc = ezdb_postings_builder_init(&entry_builder, 262144u);
        if (rc == EZDB_OK) entry_builder_ready = 1;
    }
    if (rc == EZDB_OK && entry_builder_ready && source && source->open_range && source->close_range &&
        options->index_threads > 1u && original_archive_count) {
        parallel_count_possible = 1;
    }

    double stage_start_ms = ezdb_now_ms();
    rc = ezdb_entries_collect_sections(&entry_collect,
                                       source,
                                       entry_count,
                                       original_archive_count,
                                       original_to_final,
                                       header->file_count,
                                       options->temp_dir,
                                       parallel_count_possible,
                                       (entry_builder_ready && !parallel_count_possible) ? ezdb_postings_count_entry_path_callback : NULL,
                                       &entry_builder);
    stream_total_ms = ezdb_now_ms() - stage_start_ms;
    index_count_ms = parallel_count_possible ? 0.0 : stream_total_ms;

    if (rc == EZDB_OK && entry_count && entry_builder_ready) {
        stage_start_ms = ezdb_now_ms();
        rc = ezdb_postings_build_entry_index(out,
                                             header->postings_offset,
                                             source,
                                             &entry_collect,
                                             entry_count,
                                             original_archive_count,
                                             options->index_threads,
                                             &entry_builder,
                                             &entry_index,
                                             &entry_index_stats);
        write_postings_ms = ezdb_now_ms() - stage_start_ms;
        if (rc == EZDB_OK) {
            index_count_ms = entry_index_stats.count_ms;
            index_reduce_ms = entry_index_stats.reduce_ms;
            index_prepare_ms = entry_index_stats.prepare_ms;
            index_fill_ms = entry_index_stats.fill_ms;
            write_postings_ms = entry_index_stats.write_ms;
            header->entry_index_offset = entry_index_stats.index_offset;
            header->entry_index_count = entry_index_stats.index_count;
            header->entry_postings_size = entry_index_stats.postings_size;
        }
    }
    postings_build_ms = index_count_ms + index_prepare_ms + index_fill_ms;
    if (rc == EZDB_OK) {
        rc = ezdb_entries_write_collected_sections(&entry_collect, out, header, entry_count, &entry_finalize_stats);
    }
    if (rc == EZDB_OK) {
        double total_ms = ezdb_now_ms() - total_start_ms;
        printf("entry_collect_seconds: %.3f\n", stream_total_ms / 1000.0);
        printf("stream_entry_core_seconds: %.3f\n", entry_finalize_stats.write_core_ms / 1000.0);
        printf("stream_entry_detail_seconds: %.3f\n", entry_finalize_stats.write_detail_ms / 1000.0);
        printf("stream_raw_blob_seconds: %.3f\n", entry_finalize_stats.write_raw_ms / 1000.0);
        printf("stream_postings_build_seconds: %.3f\n", postings_build_ms / 1000.0);
        printf("entry_write_core_seconds: %.3f\n", entry_finalize_stats.write_core_ms / 1000.0);
        printf("entry_write_detail_seconds: %.3f\n", entry_finalize_stats.write_detail_ms / 1000.0);
        printf("entry_write_raw_blob_seconds: %.3f\n", entry_finalize_stats.write_raw_ms / 1000.0);
        printf("entry_index_threads: %u\n", options->index_threads);
        printf("entry_index_count_seconds: %.3f\n", index_count_ms / 1000.0);
        printf("entry_index_count_parallel_seconds: %.3f\n", index_count_ms / 1000.0);
        printf("entry_index_count_parallel_enabled: %u\n", entry_index_stats.parallel_count_enabled ? 1u : 0u);
        printf("entry_index_reduce_seconds: %.3f\n", index_reduce_ms / 1000.0);
        printf("entry_index_prepare_seconds: %.3f\n", index_prepare_ms / 1000.0);
        printf("entry_index_fill_seconds: %.3f\n", index_fill_ms / 1000.0);
        printf("entry_index_fill_parallel_seconds: %.3f\n", index_fill_ms / 1000.0);
        printf("entry_index_fill_parallel_enabled: %u\n", entry_index_stats.parallel_fill_enabled ? 1u : 0u);
        printf("entry_write_postings_seconds: %.3f\n", write_postings_ms / 1000.0);
        printf("entry_index_write_seconds: %.3f\n", write_postings_ms / 1000.0);
        printf("spool_event_bytes_mb: %.2f\n", 0.0);
        printf("spool_sort_seconds: %.3f\n", 0.0);
        printf("spool_merge_seconds: %.3f\n", 0.0);
        printf("stream_peak_memory_mb: %.2f\n", ezdb_peak_working_set_mb());
        printf("entry_postings_sort_seconds: %.3f\n", entry_index_stats.write_stats.sort_ms / 1000.0);
        printf("entry_postings_choose_seconds: %.3f\n", entry_index_stats.write_stats.choose_ms / 1000.0);
        printf("entry_postings_encode_seconds: %.3f\n", entry_index_stats.write_stats.encode_ms / 1000.0);
        printf("entry_postings_compress_seconds: %.3f\n", entry_index_stats.write_stats.compress_ms / 1000.0);
        printf("entry_postings_fwrite_seconds: %.3f\n", entry_index_stats.write_stats.fwrite_ms / 1000.0);
        printf("entry_postings_index_meta_seconds: %.3f\n", entry_index_stats.write_stats.index_meta_ms / 1000.0);
        printf("entry_write_index_seconds: %.3f\n", entry_index_stats.write_index_ms / 1000.0);
        printf("entry_finalize_seconds: %.3f\n", entry_finalize_stats.finalize_ms / 1000.0);
        printf("entry_total_seconds: %.3f\n", total_ms / 1000.0);
        printf("entry_raw_blob_mb: %.2f\n", (double)entry_collect.sections.writer.raw_writer.raw_size / 1024.0 / 1024.0);
        printf("entry_records_mb: %.2f\n", (double)header->entry_records_size / 1024.0 / 1024.0);
        printf("entry_detail_mb: %.2f\n", (double)header->entry_detail_size / 1024.0 / 1024.0);
        printf("entry_postings_mb: %.2f\n", (double)entry_index_stats.postings_size / 1024.0 / 1024.0);
        printf("entry_index_count: %u\n", entry_index_stats.index_count);
        printf("entry_postings_raw_mb: %.2f\n", (double)entry_index_stats.write_stats.raw_bytes / 1024.0 / 1024.0);
        printf("entry_postings_encoded_mb: %.2f\n", (double)entry_index_stats.write_stats.encoded_bytes / 1024.0 / 1024.0);
        printf("entry_postings_array_count: %u\n", entry_index_stats.write_stats.array_count);
        printf("entry_postings_range_count: %u\n", entry_index_stats.write_stats.range_count);
        printf("entry_postings_bitset_count: %u\n", entry_index_stats.write_stats.bitset_count);
        printf("entry_postings_compressed_count: %u\n", entry_index_stats.write_stats.compressed_count);
    }
    free(entry_index);
    if (entry_builder_ready) ezdb_postings_builder_free(&entry_builder);
    ezdb_entries_collect_result_free(&entry_collect);
    return rc;
}

static int ezdb_write_entries(FILE* out,
                              EzdbHeader* header,
                              const EzdbEntryRecord* entries,
                              uint32_t entry_count,
                              uint32_t original_archive_count,
                              const uint32_t* original_to_final,
                              const EzdbBuildOptionsResolved* options)
{
    if ((!entries && entry_count)) return EZDB_ERR_ARG;
    EzdbArrayEntrySource array_source;
    EzdbEntrySource source;
    ezdb_entries_array_source_init(&source, &array_source, entries, entry_count);
    return ezdb_write_entries_from_source(out, header, &source, entry_count, original_archive_count, original_to_final, options);
}

static void ezdb_add_v13_section(EzdbSectionDesc* sections,
                                 uint32_t* section_count,
                                 uint32_t section_id,
                                 uint32_t flags,
                                 uint64_t offset,
                                 uint64_t encoded_size,
                                 uint64_t raw_size,
                                 uint64_t aux_offset,
                                 uint64_t aux_size,
                                 uint32_t page_size,
                                 uint32_t aux_count)
{
    if (!encoded_size && !raw_size && !aux_size && !aux_count) return;
    EzdbSectionDesc* section = &sections[*section_count];
    memset(section, 0, sizeof(*section));
    section->section_id = section_id;
    section->flags = flags;
    section->offset = offset;
    section->encoded_size = encoded_size;
    section->raw_size = raw_size;
    section->aux_offset = aux_offset;
    section->aux_size = aux_size;
    section->page_size = page_size;
    section->aux_count = aux_count;
    *section_count += 1u;
}

static int ezdb_build_v13_sections_from_header(const EzdbHeader* header,
                                               EzdbSectionDesc* sections,
                                               uint32_t section_cap,
                                               uint32_t* out_section_count)
{
    if (!header || !sections || !out_section_count || section_cap < EZDB_SECTION_METADATA) return EZDB_ERR_ARG;
    uint32_t count = 0;
    ezdb_add_v13_section(sections, &count, EZDB_SECTION_ARCHIVE_RECORDS,
                         header->file_records_flags,
                         header->file_records_offset,
                         header->file_records_size,
                         header->file_records_raw_size,
                         0, 0, 0, 0);
    ezdb_add_v13_section(sections, &count, EZDB_SECTION_DIR_RECORDS,
                         header->dir_records_flags,
                         header->dir_records_offset,
                         header->dir_records_size,
                         header->dir_records_raw_size,
                         0, 0, 0, 0);
    ezdb_add_v13_section(sections, &count, EZDB_SECTION_STRING_POOL,
                         header->strings_flags,
                         header->strings_offset,
                         header->strings_size,
                         header->strings_raw_size,
                         0, 0, 0, 0);
    ezdb_add_v13_section(sections, &count, EZDB_SECTION_ARCHIVE_META,
                         header->archive_meta_flags,
                         header->archive_meta_offset,
                         header->archive_meta_size,
                         header->archive_meta_raw_size,
                         0, 0, 0, 0);
    ezdb_add_v13_section(sections, &count, EZDB_SECTION_ENTRY_CORE,
                         header->entry_records_flags,
                         header->entry_records_offset,
                         header->entry_records_size,
                         header->entry_records_raw_size,
                         0, 0, 0, 0);
    ezdb_add_v13_section(sections, &count, EZDB_SECTION_ENTRY_DETAIL_PAGES,
                         0,
                         header->entry_detail_offset,
                         header->entry_detail_size,
                         sizeof(EzdbDiskEntry) * header->base_entry_count,
                         header->entry_detail_index_offset,
                         sizeof(EzdbDiskPage) * header->entry_detail_page_count,
                         header->entry_page_size,
                         (uint32_t)header->entry_detail_page_count);
    ezdb_add_v13_section(sections, &count, EZDB_SECTION_ENTRY_RAW_PAGES,
                         0,
                         header->raw_blob_offset,
                         header->raw_blob_size,
                         header->raw_blob_raw_size,
                         header->raw_blob_index_offset,
                         sizeof(EzdbDiskPage) * header->raw_blob_page_count,
                         header->raw_blob_page_size,
                         (uint32_t)header->raw_blob_page_count);
    ezdb_add_v13_section(sections, &count, EZDB_SECTION_FILE_INDEX,
                         0,
                         header->file_index_offset,
                         sizeof(EzdbDiskIndex) * header->file_index_count,
                         sizeof(EzdbDiskIndex) * header->file_index_count,
                         0, 0, 0, 0);
    ezdb_add_v13_section(sections, &count, EZDB_SECTION_DIR_INDEX,
                         0,
                         header->dir_index_offset,
                         sizeof(EzdbDiskIndex) * header->dir_index_count,
                         sizeof(EzdbDiskIndex) * header->dir_index_count,
                         0, 0, 0, 0);
    ezdb_add_v13_section(sections, &count, EZDB_SECTION_ENTRY_INDEX,
                         0,
                         header->entry_index_offset,
                         sizeof(EzdbDiskIndex) * header->entry_index_count,
                         sizeof(EzdbDiskIndex) * header->entry_index_count,
                         0, 0, 0, 0);
    ezdb_add_v13_section(sections, &count, EZDB_SECTION_POSTINGS,
                         0,
                         header->postings_offset,
                         header->postings_size + header->entry_postings_size,
                         header->postings_size + header->entry_postings_size,
                         0, 0, 0, 0);
    ezdb_add_v13_section(sections, &count, EZDB_SECTION_DELTA_LOG,
                         0,
                         header->delta_offset,
                         header->delta_size,
                         header->delta_size,
                         0, 0, 0, 0);
    *out_section_count = count;
    return EZDB_OK;
}

static int ezdb_write_v13_header_and_section_table(FILE* out, const EzdbHeader* header)
{
    if (!out || !header) return EZDB_ERR_ARG;
    EzdbSectionDesc sections[EZDB_SECTION_METADATA];
    uint32_t section_count = 0;
    int rc = ezdb_build_v13_sections_from_header(header, sections, EZDB_SECTION_METADATA, &section_count);
    if (rc != EZDB_OK) return rc;
    uint64_t table_offset = 0;
    uint64_t table_size = 0;
    rc = ezdb_format_write_section_table(out, sections, section_count, &table_offset, &table_size);
    if (rc != EZDB_OK) return rc;
    uint64_t file_size = table_offset + table_size;
    rc = ezdb_format_validate_section_table(sections, section_count, file_size);
    if (rc != EZDB_OK) return rc;

    EzdbV13Header disk_header;
    memset(&disk_header, 0, sizeof(disk_header));
    memcpy(disk_header.magic, EZDB_V13_MAGIC, sizeof(disk_header.magic));
    disk_header.version = EZDB_V13_VERSION;
    disk_header.header_size = sizeof(EzdbV13Header);
    disk_header.section_count = section_count;
    disk_header.section_table_offset = table_offset;
    disk_header.archive_count = header->file_count;
    disk_header.active_archive_count = header->active_count;
    disk_header.dir_count = header->dir_count;
    disk_header.entry_count = header->entry_count;
    disk_header.active_entry_count = header->active_entry_count;
    disk_header.base_archive_count = header->base_file_count;
    disk_header.base_entry_count = header->base_entry_count;
    if (fseek(out, 0, SEEK_SET) != 0 || fwrite(&disk_header, sizeof(disk_header), 1, out) != 1) {
        return EZDB_ERR_IO;
    }
    return EZDB_OK;
}

static int ezdb_write_v13_db_header(Ezdb* db)
{
    if (!db || !db->fp) return EZDB_ERR_ARG;
    uint64_t table_offset = db->header.delta_offset
        ? db->header.delta_offset + db->header.delta_size
        : db->v13_section_table_offset;
    if (!table_offset) table_offset = db->header.reserved_offset;
    if (fseek(db->fp, (long)table_offset, SEEK_SET) != 0) return EZDB_ERR_IO;

    EzdbSectionDesc sections[EZDB_SECTION_METADATA];
    uint32_t section_count = 0;
    int rc = ezdb_build_v13_sections_from_header(&db->header, sections, EZDB_SECTION_METADATA, &section_count);
    if (rc != EZDB_OK) return rc;
    uint64_t written_table_offset = 0;
    uint64_t table_size = 0;
    rc = ezdb_format_write_section_table(db->fp, sections, section_count, &written_table_offset, &table_size);
    if (rc != EZDB_OK) return rc;
    if (written_table_offset != table_offset) return EZDB_ERR_IO;
    uint64_t file_size = table_offset + table_size;
    rc = ezdb_format_validate_section_table(sections, section_count, file_size);
    if (rc != EZDB_OK) return rc;

    db->v13_section_table_offset = table_offset;
    db->v13_section_table_size = table_size;
    db->header.reserved_offset = file_size;
    db->header.reserved_size = 0;

    EzdbV13Header disk_header;
    memset(&disk_header, 0, sizeof(disk_header));
    memcpy(disk_header.magic, EZDB_V13_MAGIC, sizeof(disk_header.magic));
    disk_header.version = EZDB_V13_VERSION;
    disk_header.header_size = sizeof(EzdbV13Header);
    disk_header.section_count = section_count;
    disk_header.section_table_offset = table_offset;
    disk_header.archive_count = db->header.file_count;
    disk_header.active_archive_count = db->header.active_count;
    disk_header.dir_count = db->header.dir_count;
    disk_header.entry_count = db->header.entry_count;
    disk_header.active_entry_count = db->header.active_entry_count;
    disk_header.base_archive_count = db->header.base_file_count;
    disk_header.base_entry_count = db->header.base_entry_count;
    if (fseek(db->fp, 0, SEEK_SET) != 0 || fwrite(&disk_header, sizeof(disk_header), 1, db->fp) != 1) {
        return EZDB_ERR_IO;
    }
    return flush_file(db->fp);
}

static uint64_t ezdb_delta_append_offset(Ezdb* db)
{
    if (!db) return 0;
    if (db->format_v13) {
        if (db->header.delta_offset && db->header.delta_size) return db->header.delta_offset + db->header.delta_size;
        return db->v13_section_table_offset ? db->v13_section_table_offset : db->header.reserved_offset;
    }
    return db->header.delta_offset ? db->header.delta_offset + db->header.delta_size : db->header.reserved_offset;
}

static int ezdb_get_file_size(FILE* fp, uint64_t* out_size)
{
    if (!fp || !out_size) return EZDB_ERR_ARG;
    long current = ftell(fp);
    if (current < 0) return EZDB_ERR_IO;
    if (fseek(fp, 0, SEEK_END) != 0) return EZDB_ERR_IO;
    long end = ftell(fp);
    if (end < 0) return EZDB_ERR_IO;
    if (fseek(fp, current, SEEK_SET) != 0) return EZDB_ERR_IO;
    *out_size = (uint64_t)end;
    return EZDB_OK;
}

static uint64_t ezdb_v13_index_count_from_section(const EzdbSectionDesc* section)
{
    if (!section) return 0;
    if (section->aux_count) return section->aux_count;
    if (sizeof(EzdbDiskIndex) && section->encoded_size % sizeof(EzdbDiskIndex) == 0) {
        return section->encoded_size / sizeof(EzdbDiskIndex);
    }
    return UINT64_MAX;
}

static int ezdb_apply_v13_section_header(EzdbHeader* header,
                                         const EzdbSectionDesc* sections,
                                         uint32_t section_count,
                                         uint32_t section_id)
{
    const EzdbSectionDesc* section = ezdb_format_find_section(sections, section_count, section_id);
    if (!section) return EZDB_OK;
    switch (section_id) {
    case EZDB_SECTION_ARCHIVE_RECORDS:
        header->file_records_offset = section->offset;
        header->file_records_size = section->encoded_size;
        header->file_records_raw_size = section->raw_size;
        header->file_records_flags = section->flags;
        break;
    case EZDB_SECTION_DIR_RECORDS:
        header->dir_records_offset = section->offset;
        header->dir_records_size = section->encoded_size;
        header->dir_records_raw_size = section->raw_size;
        header->dir_records_flags = section->flags;
        break;
    case EZDB_SECTION_STRING_POOL:
        header->strings_offset = section->offset;
        header->strings_size = section->encoded_size;
        header->strings_raw_size = section->raw_size;
        header->strings_flags = section->flags;
        break;
    case EZDB_SECTION_ARCHIVE_META:
        header->archive_meta_offset = section->offset;
        header->archive_meta_size = section->encoded_size;
        header->archive_meta_raw_size = section->raw_size;
        header->archive_meta_flags = section->flags;
        break;
    case EZDB_SECTION_ENTRY_CORE:
        header->entry_records_offset = section->offset;
        header->entry_records_size = section->encoded_size;
        header->entry_records_raw_size = section->raw_size;
        header->entry_records_flags = section->flags;
        break;
    case EZDB_SECTION_ENTRY_DETAIL_PAGES:
        header->entry_detail_offset = section->offset;
        header->entry_detail_size = section->encoded_size;
        header->entry_detail_index_offset = section->aux_offset;
        header->entry_detail_page_count = section->aux_count;
        header->entry_page_size = section->page_size;
        break;
    case EZDB_SECTION_ENTRY_RAW_PAGES:
        header->raw_blob_offset = section->offset;
        header->raw_blob_size = section->encoded_size;
        header->raw_blob_raw_size = section->raw_size;
        header->raw_blob_index_offset = section->aux_offset;
        header->raw_blob_page_count = section->aux_count;
        header->raw_blob_page_size = section->page_size;
        break;
    case EZDB_SECTION_FILE_INDEX: {
        uint64_t count = ezdb_v13_index_count_from_section(section);
        if (count == UINT64_MAX) return EZDB_ERR_FORMAT;
        header->file_index_offset = section->offset;
        header->file_index_count = count;
        break;
    }
    case EZDB_SECTION_DIR_INDEX: {
        uint64_t count = ezdb_v13_index_count_from_section(section);
        if (count == UINT64_MAX) return EZDB_ERR_FORMAT;
        header->dir_index_offset = section->offset;
        header->dir_index_count = count;
        break;
    }
    case EZDB_SECTION_ENTRY_INDEX: {
        uint64_t count = ezdb_v13_index_count_from_section(section);
        if (count == UINT64_MAX) return EZDB_ERR_FORMAT;
        header->entry_index_offset = section->offset;
        header->entry_index_count = count;
        break;
    }
    case EZDB_SECTION_POSTINGS:
        header->postings_offset = section->offset;
        header->postings_size = section->encoded_size;
        header->entry_postings_size = 0;
        break;
    case EZDB_SECTION_DELTA_LOG:
        header->delta_offset = section->offset;
        header->delta_size = section->encoded_size;
        break;
    default:
        break;
    }
    return EZDB_OK;
}

static int ezdb_apply_v13_header(EzdbHeader* header,
                                 const EzdbV13Header* disk_header,
                                 const EzdbSectionDesc* sections,
                                 uint32_t section_count,
                                 uint64_t file_size)
{
    if (!header || !disk_header || !sections) return EZDB_ERR_ARG;
    memset(header, 0, sizeof(*header));
    memcpy(header->magic, EZDB_MAGIC, sizeof(header->magic));
    header->version = EZDB_VERSION;
    header->header_size = sizeof(EzdbHeader);
    header->file_count = disk_header->archive_count;
    header->active_count = disk_header->active_archive_count;
    header->dir_count = disk_header->dir_count;
    header->entry_count = disk_header->entry_count;
    header->active_entry_count = disk_header->active_entry_count;
    header->base_file_count = disk_header->base_archive_count;
    header->base_entry_count = disk_header->base_entry_count;
    header->reserved_offset = file_size;
    header->reserved_size = 0;

    for (uint32_t i = 0; i < section_count; ++i) {
        int rc = ezdb_apply_v13_section_header(header, sections, section_count, sections[i].section_id);
        if (rc != EZDB_OK) return rc;
    }
    return EZDB_OK;
}

static int ezdb_write_archive_base(const EzdbArchiveRecord* archives,
                                   uint32_t archive_count,
                                   const EzdbEntryRecord* entries,
                                   uint32_t entry_count,
                                   const char* output_ezdb,
                                   uint32_t* original_to_final,
                                   EzdbEntrySource* entry_source,
                                   const EzdbBuildOptionsResolved* options)
{
    if ((!archives && archive_count) || (!entries && !entry_source && entry_count) || !output_ezdb) return EZDB_ERR_ARG;
    double total_start_ms = ezdb_now_ms();
    double build_tree_ms = 0.0;
    double dfs_ms = 0.0;
    double write_base_ms = 0.0;
    double file_index_ms = 0.0;
    double dir_index_ms = 0.0;

    BuildDir* dirs = NULL;
    BuildFile* old_files = NULL;
    uint32_t dir_count = 0, dir_cap = 0, file_count = 0, file_cap = 0;
    DirHashEntry* dir_hash_entries = NULL;
    uint32_t dir_hash_count = 0, dir_hash_cap = 0, dir_bucket_count = 0;
    uint32_t* dir_buckets = NULL;
    char* string_pool = NULL;
    uint32_t string_size = 0, string_cap = 0;
    StringHashEntry* string_entries = NULL;
    uint32_t string_entry_count = 0, string_entry_cap = 0, string_bucket_count = 0;
    uint32_t* string_buckets = NULL;
    uint32_t* file_name_offsets = NULL;
    PostingBuilder file_builder;
    PostingBuilder dir_builder;
    int file_builder_ready = 0;
    int dir_builder_ready = 0;
    int rc = EZDB_OK;

    if (ensure_capacity((void**)&dirs, sizeof(BuildDir), &dir_cap, 1) != EZDB_OK) {
        return EZDB_ERR_MEMORY;
    }
    memset(&dirs[0], 0, sizeof(BuildDir));
    dirs[0].parent = 0;
    dirs[0].first_child = UINT32_MAX;
    dirs[0].next_sibling = UINT32_MAX;
    dirs[0].first_file = UINT32_MAX;
    dirs[0].old_first_file = UINT32_MAX;
    dir_count = 1;

    double stage_start_ms = ezdb_now_ms();
    for (uint32_t i = 0; i < archive_count; ++i) {
        const EzdbArchiveRecord* archive = &archives[i];
        const char* path = archive->file_path;
        if (!path || !*path) {
            rc = EZDB_ERR_ARG;
            break;
        }
        char* slash = strrchr(path, '\\');
        char* fslash = strrchr(path, '/');
        if (!slash || (fslash && fslash > slash)) slash = fslash;
        const char* name = slash ? slash + 1 : path;
        uint32_t name_len = (uint32_t)strlen(name);
        uint32_t dir_id = 0;
        if (slash) {
            dir_id = get_or_create_path_dir(&dirs, &dir_count, &dir_cap,
                                            &dir_hash_entries, &dir_hash_count, &dir_hash_cap,
                                            &dir_buckets, &dir_bucket_count,
                                            &string_pool, &string_size, &string_cap,
                                            &string_entries, &string_entry_count, &string_entry_cap,
                                            &string_buckets, &string_bucket_count,
                                            path, (uint32_t)(slash - path));
            if (dir_id == UINT32_MAX) {
                rc = EZDB_ERR_MEMORY;
                break;
            }
        }
        rc = append_file(&old_files, &file_count, &file_cap, dirs, dir_id,
                         &string_pool, &string_size, &string_cap,
                         &string_entries, &string_entry_count, &string_entry_cap,
                         &string_buckets, &string_bucket_count,
                         name, name_len, i, archive->file_size, archive->modified_time,
                         archive->drive_letter, archive->file_ref_number, archive->usn);
        if (rc != EZDB_OK) break;
    }
    build_tree_ms = ezdb_now_ms() - stage_start_ms;

    BuildFile* files = NULL;
    if (rc == EZDB_OK) {
        files = (BuildFile*)malloc(sizeof(BuildFile) * (size_t)file_count);
        if (!files && file_count) rc = EZDB_ERR_MEMORY;
    }
    if (rc == EZDB_OK) {
        stage_start_ms = ezdb_now_ms();
        uint32_t assigned = dfs_assign(dirs, old_files, files, 0, 0, original_to_final);
        if (assigned != file_count) rc = EZDB_ERR_FORMAT;
        free(old_files);
        old_files = NULL;
        dfs_ms = ezdb_now_ms() - stage_start_ms;
    }
    if (rc == EZDB_OK) {
        FILE* out = fopen(output_ezdb, "wb");
        if (!out) rc = EZDB_ERR_IO;
        if (out) {
            EzdbHeader header;
            memset(&header, 0, sizeof(header));
            memcpy(header.magic, EZDB_MAGIC, 8);
            header.version = EZDB_VERSION;
            header.header_size = sizeof(EzdbHeader);
            header.file_count = file_count;
            header.active_count = file_count;
            header.base_file_count = file_count;
            header.dir_count = dir_count;
            EzdbV13Header disk_header_placeholder;
            memset(&disk_header_placeholder, 0, sizeof(disk_header_placeholder));
            if (fwrite(&disk_header_placeholder, sizeof(disk_header_placeholder), 1, out) != 1) rc = EZDB_ERR_IO;

            stage_start_ms = ezdb_now_ms();
            header.file_records_offset = (uint64_t)ftell(out);
            unsigned char* file_records_raw = NULL;
            uint64_t file_records_raw_size = 0;
            uint64_t file_records_written = 0;
            rc = encode_file_records_compact(files, file_count, &file_records_raw, &file_records_raw_size);
            if (rc == EZDB_OK) rc = write_compressed_section(out, file_records_raw, file_records_raw_size, &file_records_written, &header.file_records_flags);
            free(file_records_raw);
            header.file_records_raw_size = file_records_raw_size;
            header.file_records_size = file_records_written;
            if (rc == EZDB_OK) {
                file_name_offsets = (uint32_t*)malloc(sizeof(uint32_t) * (size_t)(file_count ? file_count : 1u));
                if (!file_name_offsets) {
                    rc = EZDB_ERR_MEMORY;
                } else {
                    for (uint32_t i = 0; i < file_count; ++i) file_name_offsets[i] = files[i].name_offset;
                }
            }
            if (rc == EZDB_OK) {
                header.archive_meta_offset = (uint64_t)ftell(out);
                uint64_t archive_meta_raw_size = sizeof(EzdbDiskArchiveMeta) * (uint64_t)file_count;
                EzdbDiskArchiveMeta* archive_meta_raw = (EzdbDiskArchiveMeta*)calloc((size_t)(file_count ? file_count : 1u), sizeof(EzdbDiskArchiveMeta));
                if (!archive_meta_raw) {
                    rc = EZDB_ERR_MEMORY;
                } else {
                    for (uint32_t i = 0; i < file_count; ++i) {
                        archive_meta_raw[i].file_ref_number = files[i].file_ref_number;
                        archive_meta_raw[i].usn = files[i].usn;
                        archive_meta_raw[i].drive_letter = (unsigned char)files[i].drive_letter;
                    }
                    uint64_t archive_meta_written = 0;
                    rc = write_compressed_section(out, (const unsigned char*)archive_meta_raw, archive_meta_raw_size, &archive_meta_written, &header.archive_meta_flags);
                    header.archive_meta_raw_size = archive_meta_raw_size;
                    header.archive_meta_size = archive_meta_written;
                    free(archive_meta_raw);
                }
            }
            free(files);
            files = NULL;

            header.dir_records_offset = (uint64_t)ftell(out);
            uint64_t dir_records_raw_size = sizeof(EzdbDiskDir) * (uint64_t)dir_count;
            EzdbDiskDir* dir_records_raw = (EzdbDiskDir*)malloc(dir_records_raw_size ? (size_t)dir_records_raw_size : 1u);
            if (!dir_records_raw) rc = EZDB_ERR_MEMORY;
            for (uint32_t i = 0; i < dir_count && rc == EZDB_OK; ++i) {
                dir_records_raw[i].parent_dir_id = dirs[i].parent;
                dir_records_raw[i].name_offset = dirs[i].name_offset;
                dir_records_raw[i].name_len = dirs[i].name_len;
                dir_records_raw[i].first_file_id = dirs[i].first_file_id;
                dir_records_raw[i].file_count = dirs[i].file_count;
            }
            uint64_t dir_records_written = 0;
            if (rc == EZDB_OK) rc = write_compressed_section(out, (const unsigned char*)dir_records_raw, dir_records_raw_size, &dir_records_written, &header.dir_records_flags);
            free(dir_records_raw);
            header.dir_records_raw_size = dir_records_raw_size;
            header.dir_records_size = dir_records_written;

            header.strings_offset = (uint64_t)ftell(out);
            header.strings_raw_size = string_size;
            uint64_t strings_written = 0;
            if (rc == EZDB_OK) rc = write_compressed_section(out, (const unsigned char*)string_pool, string_size, &strings_written, &header.strings_flags);
            header.strings_size = strings_written;
            write_base_ms = ezdb_now_ms() - stage_start_ms;

            free(dir_hash_entries);
            dir_hash_entries = NULL;
            dir_hash_count = 0;
            dir_hash_cap = 0;
            free(dir_buckets);
            dir_buckets = NULL;
            dir_bucket_count = 0;
            free(string_entries);
            string_entries = NULL;
            string_entry_count = 0;
            string_entry_cap = 0;
            free(string_buckets);
            string_buckets = NULL;
            string_bucket_count = 0;

            header.postings_offset = (uint64_t)ftell(out);
            EzdbDiskIndex* file_index = NULL;
            EzdbDiskIndex* dir_index = NULL;
            uint32_t file_index_count = 0, dir_index_count = 0;
            uint64_t file_postings_size = 0, dir_postings_size = 0;
            if (rc == EZDB_OK) {
                stage_start_ms = ezdb_now_ms();
                rc = ezdb_postings_builder_init(&file_builder, 262144u);
                if (rc == EZDB_OK) file_builder_ready = 1;
            }
            if (rc == EZDB_OK) {
                for (uint32_t i = 0; i < file_count; ++i) {
                    rc = ezdb_postings_count_text_grams(&file_builder, string_pool + file_name_offsets[i], i);
                    if (rc != EZDB_OK) break;
                }
            }
            if (rc == EZDB_OK) rc = ezdb_postings_builder_prepare_fill(&file_builder);
            if (rc == EZDB_OK) {
                for (uint32_t i = 0; i < file_count; ++i) {
                    rc = ezdb_postings_fill_text_grams(&file_builder, string_pool + file_name_offsets[i], i);
                    if (rc != EZDB_OK) break;
                }
            }
            if (rc == EZDB_OK) rc = ezdb_postings_write(out, &file_builder, file_count, &file_index, &file_index_count, &file_postings_size, NULL);
            if (rc == EZDB_OK) file_index_ms = ezdb_now_ms() - stage_start_ms;
            if (file_builder_ready) {
                ezdb_postings_builder_free(&file_builder);
                file_builder_ready = 0;
            }
            if (rc == EZDB_OK) {
                stage_start_ms = ezdb_now_ms();
                rc = ezdb_postings_builder_init(&dir_builder, 131072u);
                if (rc == EZDB_OK) dir_builder_ready = 1;
            }
            if (rc == EZDB_OK) {
                for (uint32_t i = 1; i < dir_count; ++i) {
                    rc = ezdb_postings_count_text_grams(&dir_builder, string_pool + dirs[i].name_offset, i);
                    if (rc != EZDB_OK) break;
                }
            }
            if (rc == EZDB_OK) rc = ezdb_postings_builder_prepare_fill(&dir_builder);
            if (rc == EZDB_OK) {
                for (uint32_t i = 1; i < dir_count; ++i) {
                    rc = ezdb_postings_fill_text_grams(&dir_builder, string_pool + dirs[i].name_offset, i);
                    if (rc != EZDB_OK) break;
                }
            }
            if (rc == EZDB_OK) rc = ezdb_postings_write(out, &dir_builder, dir_count, &dir_index, &dir_index_count, &dir_postings_size, NULL);
            if (rc == EZDB_OK) dir_index_ms = ezdb_now_ms() - stage_start_ms;
            if (dir_builder_ready) {
                ezdb_postings_builder_free(&dir_builder);
                dir_builder_ready = 0;
            }
            header.postings_size = file_postings_size + dir_postings_size;

            if (rc == EZDB_OK) {
                if (entry_source) {
                    rc = ezdb_write_entries_from_source(out, &header, entry_source, entry_count, archive_count, original_to_final, options);
                } else {
                    rc = ezdb_write_entries(out, &header, entries, entry_count, archive_count, original_to_final, options);
                }
            }

            header.file_index_offset = (uint64_t)ftell(out);
            header.file_index_count = file_index_count;
            if (rc == EZDB_OK && file_index_count && fwrite(file_index, sizeof(EzdbDiskIndex), file_index_count, out) != file_index_count) rc = EZDB_ERR_IO;

            for (uint32_t i = 0; i < dir_index_count; ++i) dir_index[i].offset += file_postings_size;
            header.dir_index_offset = (uint64_t)ftell(out);
            header.dir_index_count = dir_index_count;
            if (rc == EZDB_OK && dir_index_count && fwrite(dir_index, sizeof(EzdbDiskIndex), dir_index_count, out) != dir_index_count) rc = EZDB_ERR_IO;

            if (rc == EZDB_OK) rc = ezdb_write_v13_header_and_section_table(out, &header);
            free(file_index);
            free(dir_index);
            fclose(out);
        }
    }

    free(dirs);
    free(old_files);
    free(files);
    free(file_name_offsets);
    free(dir_hash_entries);
    free(dir_buckets);
    free(string_pool);
    free(string_entries);
    free(string_buckets);
    if (file_builder_ready) ezdb_postings_builder_free(&file_builder);
    if (dir_builder_ready) ezdb_postings_builder_free(&dir_builder);
    if (rc == EZDB_OK) {
        double total_ms = ezdb_now_ms() - total_start_ms;
        printf("build_build_tree_seconds: %.3f\n", build_tree_ms / 1000.0);
        printf("build_dfs_seconds: %.3f\n", dfs_ms / 1000.0);
        printf("build_write_base_seconds: %.3f\n", write_base_ms / 1000.0);
        printf("build_file_index_seconds: %.3f\n", file_index_ms / 1000.0);
        printf("build_dir_index_seconds: %.3f\n", dir_index_ms / 1000.0);
        printf("build_internal_total_seconds: %.3f\n", total_ms / 1000.0);
    }
    return rc;
}

static int entry_is_searchable(Ezdb* db, uint32_t entry_id)
{
    if (!db || entry_id >= db->header.entry_count || !bitset_get(db->active_entry_bits, entry_id)) return 0;
    uint32_t archive_id = db->entry_archive_ids[entry_id];
    return archive_id < db->header.file_count && bitset_get(db->active_bits, archive_id);
}

static int mark_entry_literal_candidates(Ezdb* db, const char* literal, size_t literal_len, unsigned char* seen, int* any_marked)
{
    int rc = EZDB_OK;
    if (db->entry_index && db->header.entry_index_count) {
        char* keyword = ezdb_strdup_range(literal, literal_len);
        if (!keyword) return EZDB_ERR_MEMORY;
        uint32_t* keys = NULL;
        uint32_t key_count = 0;
        rc = ezdb_query_build_candidate_keys(keyword, literal_len, &keys, &key_count);
        if (rc != EZDB_OK) {
            free(keyword);
            return rc;
        }
        if (key_count) {
            uint32_t* entry_ids = NULL;
            uint32_t entry_count = 0;
            rc = ezdb_postings_load_intersected(db->fp, db->header.postings_offset, db->entry_index, db->header.entry_index_count, keys, key_count, &entry_ids, &entry_count);
            if (rc != EZDB_OK) {
                free(entry_ids);
                free(keys);
                free(keyword);
                return rc;
            }
            for (uint32_t i = 0; i < entry_count; ++i) {
                uint32_t id = entry_ids[i];
                if (entry_is_searchable(db, id)) {
                    seen[id >> 3u] |= (unsigned char)(1u << (id & 7u));
                    *any_marked = 1;
                }
            }
            free(entry_ids);
        }
        free(keys);
        free(keyword);
    }
    if (db->delta_entry_index_ready && db->delta_entry_index.entry_count) {
        char* keyword = ezdb_strdup_range(literal, literal_len);
        if (!keyword) return EZDB_ERR_MEMORY;
        uint32_t* keys = NULL;
        uint32_t key_count = 0;
        rc = ezdb_query_build_candidate_keys(keyword, literal_len, &keys, &key_count);
        if (rc != EZDB_OK) {
            free(keyword);
            return rc;
        }
        if (key_count) {
            uint32_t* entry_ids = NULL;
            uint32_t entry_count = 0;
            rc = ezdb_postings_load_intersected_memory(&db->delta_entry_index, keys, key_count, &entry_ids, &entry_count);
            if (rc != EZDB_OK) {
                free(entry_ids);
                free(keys);
                free(keyword);
                return rc;
            }
            for (uint32_t i = 0; i < entry_count; ++i) {
                uint32_t id = entry_ids[i];
                if (entry_is_searchable(db, id) && bitset_get(db->delta_entry_bits, id)) {
                    seen[id >> 3u] |= (unsigned char)(1u << (id & 7u));
                    *any_marked = 1;
                }
            }
            free(entry_ids);
        }
        free(keys);
        free(keyword);
    }
    return EZDB_OK;
}

static int mark_archive_literal_entry_candidates(Ezdb* db, const char* literal, size_t literal_len, unsigned char* seen, int* any_marked)
{
    size_t archive_bit_bytes = ((size_t)db->header.file_count + 7u) / 8u;
    unsigned char* archive_bits = (unsigned char*)calloc(archive_bit_bytes ? archive_bit_bytes : 1u, 1);
    if (!archive_bits) return EZDB_ERR_MEMORY;
    int archive_marked = 0;
    int rc = mark_literal_candidates(db, literal, literal_len, archive_bits, &archive_marked);
    if (rc != EZDB_OK) {
        free(archive_bits);
        return rc;
    }
    if (archive_marked) {
        for (uint32_t i = 0; i < db->header.entry_count; ++i) {
            if (entry_is_searchable(db, i) &&
                (archive_bits[db->entry_archive_ids[i] >> 3u] & (unsigned char)(1u << (db->entry_archive_ids[i] & 7u)))) {
                seen[i >> 3u] |= (unsigned char)(1u << (i & 7u));
                *any_marked = 1;
            }
        }
    }
    free(archive_bits);
    return EZDB_OK;
}

static int append_blob(unsigned char** data, uint32_t* size, uint32_t* cap, const void* bytes, uint32_t len, uint32_t extra_nul, uint32_t* out_offset)
{
    if (ensure_capacity((void**)data, 1, cap, *size + len + extra_nul) != EZDB_OK) return EZDB_ERR_MEMORY;
    *out_offset = *size;
    if (len) memcpy(*data + *size, bytes, len);
    *size += len;
    if (extra_nul) (*data)[(*size)++] = '\0';
    return EZDB_OK;
}

int ezdb_build_snapshot(const EzdbArchiveRecord* archives,
                        uint32_t archive_count,
                        const EzdbEntryRecord* entries,
                        uint32_t entry_count,
                        const char* output_ezdb)
{
    if ((!archives && archive_count) || (!entries && entry_count) || !output_ezdb) return EZDB_ERR_ARG;
    int rc = EZDB_OK;
    uint32_t* archive_id_map = NULL;
    if (archive_count) {
        archive_id_map = (uint32_t*)malloc(sizeof(uint32_t) * (size_t)(archive_count ? archive_count : 1u));
        if (!archive_id_map) {
            rc = EZDB_ERR_MEMORY;
        } else {
            for (uint32_t i = 0; i < archive_count; ++i) archive_id_map[i] = UINT32_MAX;
        }
    }
    EzdbBuildOptionsResolved options;
    if (rc == EZDB_OK) rc = resolve_build_options(output_ezdb, NULL, &options);
    if (rc == EZDB_OK) rc = ezdb_write_archive_base(archives, archive_count, entries, entry_count, output_ezdb, archive_id_map, NULL, &options);
    free(archive_id_map);
    if (rc != EZDB_OK) remove(output_ezdb);
    return rc;
}

int ezdb_build_snapshot_stream_entries(const EzdbArchiveRecord* archives,
                                       uint32_t archive_count,
                                       EzdbEntryStream* entry_stream,
                                       uint32_t entry_count,
                                       const char* output_ezdb)
{
    return ezdb_build_snapshot_stream_entries_ex(archives, archive_count, entry_stream, entry_count, output_ezdb, NULL);
}

typedef struct EzdbPublicEntryStreamRange {
    EzdbEntryStream stream;
} EzdbPublicEntryStreamRange;

static void public_entry_stream_close_range(EzdbEntrySource* source);

static int public_entry_stream_reset(void* user_data)
{
    EzdbEntryStream* stream = (EzdbEntryStream*)user_data;
    if (!stream) return EZDB_ERR_ARG;
    return stream->reset ? stream->reset(stream->user_data) : EZDB_OK;
}

static int public_entry_stream_reset_range(void* user_data, uint32_t archive_begin, uint32_t archive_end)
{
    EzdbEntryStream* stream = (EzdbEntryStream*)user_data;
    if (!stream || !stream->reset_range) return EZDB_ERR_ARG;
    return stream->reset_range(stream->user_data, archive_begin, archive_end);
}

static int public_entry_stream_next(void* user_data, EzdbEntryRecord* out_record)
{
    EzdbEntryStream* stream = (EzdbEntryStream*)user_data;
    if (!stream || !stream->next) return EZDB_ERR_ARG;
    return stream->next(stream->user_data, out_record);
}

static int public_entry_stream_open_range(void* user_data, uint32_t archive_begin, uint32_t archive_end, EzdbEntrySource* out_source)
{
    EzdbEntryStream* stream = (EzdbEntryStream*)user_data;
    if (!stream || !stream->open_range || !out_source) return EZDB_ERR_ARG;
    EzdbPublicEntryStreamRange* range = (EzdbPublicEntryStreamRange*)calloc(1, sizeof(*range));
    if (!range) return EZDB_ERR_MEMORY;
    int rc = stream->open_range(stream->user_data, archive_begin, archive_end, &range->stream);
    if (rc != EZDB_OK) {
        free(range);
        return rc;
    }
    memset(out_source, 0, sizeof(*out_source));
    out_source->user_data = &range->stream;
    out_source->reset = public_entry_stream_reset;
    out_source->reset_range = range->stream.reset_range ? public_entry_stream_reset_range : NULL;
    out_source->next = public_entry_stream_next;
    if (range->stream.open_range) out_source->open_range = public_entry_stream_open_range;
    out_source->close_range = public_entry_stream_close_range;
    return EZDB_OK;
}

static void public_entry_stream_close_range(EzdbEntrySource* source)
{
    if (!source) return;
    EzdbPublicEntryStreamRange* range = (EzdbPublicEntryStreamRange*)source->user_data;
    if (range && range->stream.close_range) range->stream.close_range(&range->stream);
    free(range);
    memset(source, 0, sizeof(*source));
}

int ezdb_build_snapshot_stream_entries_ex(const EzdbArchiveRecord* archives,
                                          uint32_t archive_count,
                                          EzdbEntryStream* entry_stream,
                                          uint32_t entry_count,
                                          const char* output_ezdb,
                                          const EzdbBuildOptions* build_options)
{
    if ((!archives && archive_count) || (!entry_stream && entry_count) || !output_ezdb) return EZDB_ERR_ARG;
    if (entry_count && !entry_stream->next) return EZDB_ERR_ARG;
    int rc = EZDB_OK;
    uint32_t* archive_id_map = NULL;
    EzdbBuildOptionsResolved options;
    rc = resolve_build_options(output_ezdb, build_options, &options);
    if (archive_count) {
        archive_id_map = (uint32_t*)malloc(sizeof(uint32_t) * (size_t)(archive_count ? archive_count : 1u));
        if (!archive_id_map) {
            rc = EZDB_ERR_MEMORY;
        } else {
            for (uint32_t i = 0; i < archive_count; ++i) archive_id_map[i] = UINT32_MAX;
        }
    }
    EzdbEntrySource source;
    memset(&source, 0, sizeof(source));
    if (entry_stream) {
        source.user_data = entry_stream;
        source.reset = public_entry_stream_reset;
        source.reset_range = entry_stream->reset_range ? public_entry_stream_reset_range : NULL;
        source.next = public_entry_stream_next;
        if (entry_stream->open_range) {
            source.open_range = public_entry_stream_open_range;
            source.close_range = public_entry_stream_close_range;
        }
    }
    if (rc == EZDB_OK) rc = ezdb_write_archive_base(archives, archive_count, NULL, entry_count, output_ezdb, archive_id_map, entry_count ? &source : NULL, &options);
    free(archive_id_map);
    if (rc != EZDB_OK) remove(output_ezdb);
    return rc;
}

int ezdb_open(const char* path, Ezdb** out_db)
{
    if (!path || !out_db) return EZDB_ERR_ARG;
    *out_db = NULL;
    FILE* fp = fopen(path, "r+b");
    int read_only = 0;
    if (!fp) {
        fp = fopen(path, "rb");
        read_only = 1;
    }
    if (!fp) return EZDB_ERR_IO;
    Ezdb* db = (Ezdb*)calloc(1, sizeof(Ezdb));
    if (!db) {
        fclose(fp);
        return EZDB_ERR_MEMORY;
    }
    db->fp = fp;
    db->read_only = read_only;
    db->path = ezdb_strdup_range(path, strlen(path));
    char magic[8];
    if (fread(magic, 1, sizeof(magic), fp) != sizeof(magic)) {
        ezdb_close(db);
        return EZDB_ERR_FORMAT;
    }
    if (memcmp(magic, EZDB_V13_MAGIC, sizeof(magic)) != 0) {
        ezdb_close(db);
        return EZDB_ERR_FORMAT;
    }
    if (fseek(fp, 0, SEEK_SET) != 0) {
        ezdb_close(db);
        return EZDB_ERR_IO;
    }
    EzdbV13Header disk_header;
    if (fread(&disk_header, sizeof(disk_header), 1, fp) != 1 ||
        !ezdb_format_v13_header_is_current(&disk_header)) {
        ezdb_close(db);
        return EZDB_ERR_FORMAT;
    }
    uint64_t file_size = 0;
    int rc = ezdb_get_file_size(fp, &file_size);
    if (rc != EZDB_OK) {
        ezdb_close(db);
        return rc;
    }
    EzdbSectionDesc* sections = NULL;
    rc = ezdb_format_read_section_table(fp, &disk_header, file_size, &sections);
    if (rc == EZDB_OK) rc = ezdb_apply_v13_header(&db->header, &disk_header, sections, disk_header.section_count, file_size);
    free(sections);
    if (rc != EZDB_OK) {
        ezdb_close(db);
        return rc;
    }
    db->format_v13 = 1;
    db->v13_section_table_offset = disk_header.section_table_offset;
    db->v13_section_table_size = sizeof(EzdbSectionDesc) * (uint64_t)disk_header.section_count;
    if (!db->header.base_file_count) db->header.base_file_count = db->header.file_count;
    if (!db->format_v13 && !db->header.delta_offset) db->header.delta_offset = db->header.reserved_offset;
    if (db->header.file_count > UINT32_MAX || db->header.base_file_count > UINT32_MAX ||
        db->header.dir_count > UINT32_MAX ||
        db->header.entry_count > UINT32_MAX || db->header.base_entry_count > UINT32_MAX ||
        db->header.base_entry_count > db->header.entry_count ||
        db->header.file_index_count > UINT32_MAX || db->header.dir_index_count > UINT32_MAX ||
        db->header.entry_index_count > UINT32_MAX ||
        db->header.entry_detail_page_count > UINT32_MAX || db->header.raw_blob_page_count > UINT32_MAX ||
        db->header.strings_size > UINT32_MAX || db->header.strings_raw_size > UINT32_MAX ||
        db->header.dir_records_raw_size > UINT32_MAX || db->header.file_records_raw_size > UINT32_MAX ||
        db->header.archive_meta_raw_size > UINT32_MAX || db->header.entry_records_raw_size > UINT32_MAX ||
        db->header.raw_blob_raw_size > UINT32_MAX) {
        ezdb_close(db);
        return EZDB_ERR_FORMAT;
    }
    if ((db->header.base_entry_count && db->header.entry_page_size != EZDB_ENTRY_PAGE_SIZE) ||
        (db->header.raw_blob_raw_size && db->header.raw_blob_page_size != EZDB_RAW_BLOB_PAGE_SIZE)) {
        ezdb_close(db);
        return EZDB_ERR_FORMAT;
    }
    if (!db->header.file_records_raw_size) db->header.file_records_raw_size = db->header.file_records_size;
    if (!db->header.dir_records_raw_size) db->header.dir_records_raw_size = db->header.dir_records_size;
    if (!db->header.strings_raw_size) db->header.strings_raw_size = db->header.strings_size;
    if (!db->header.archive_meta_raw_size) db->header.archive_meta_raw_size = sizeof(EzdbDiskArchiveMeta) * db->header.base_file_count;
    if (!db->header.entry_records_raw_size) db->header.entry_records_raw_size = EZDB_ENTRY_CORE_RECORD_SIZE * db->header.base_entry_count;
    db->file_parent_dir_ids = (uint32_t*)malloc(sizeof(uint32_t) * (size_t)db->header.base_file_count);
    db->file_name_offsets = (uint32_t*)malloc(sizeof(uint32_t) * (size_t)db->header.base_file_count);
    db->file_name_lens = (uint16_t*)malloc(sizeof(uint16_t) * (size_t)db->header.base_file_count);
    db->file_sizes32 = (uint32_t*)malloc(sizeof(uint32_t) * (size_t)db->header.base_file_count);
    db->file_modified_times32 = (uint32_t*)malloc(sizeof(uint32_t) * (size_t)db->header.base_file_count);
    db->archive_meta = (EzdbDiskArchiveMeta*)calloc((size_t)(db->header.base_file_count ? db->header.base_file_count : 1u), sizeof(EzdbDiskArchiveMeta));
    db->entry_archive_ids = (uint32_t*)malloc(sizeof(uint32_t) * (size_t)db->header.entry_count);
    db->entry_path_offsets = (uint32_t*)malloc(sizeof(uint32_t) * (size_t)db->header.entry_count);
    db->entry_path_lens = (uint32_t*)malloc(sizeof(uint32_t) * (size_t)db->header.entry_count);
    db->entry_next_in_archive = (uint32_t*)malloc(sizeof(uint32_t) * (size_t)(db->header.entry_count ? db->header.entry_count : 1u));
    db->archive_first_entry_ids = (uint32_t*)malloc(sizeof(uint32_t) * (size_t)(db->header.file_count ? db->header.file_count : 1u));
    db->delta_entry_refs = (EzdbDeltaEntryRef*)calloc((size_t)(db->header.entry_count ? db->header.entry_count : 1u), sizeof(EzdbDeltaEntryRef));
    size_t logical_bit_bytes = ((size_t)db->header.file_count + 7u) / 8u;
    size_t entry_bit_bytes = ((size_t)db->header.entry_count + 7u) / 8u;
    size_t base_bit_bytes = ((size_t)db->header.base_file_count + 7u) / 8u;
    db->active_bits_cap_bytes = logical_bit_bytes ? logical_bit_bytes : 1u;
    db->active_entry_bits_cap_bytes = entry_bit_bytes ? entry_bit_bytes : 1u;
    db->active_bits = (unsigned char*)malloc(db->active_bits_cap_bytes);
    db->active_entry_bits = (unsigned char*)malloc(db->active_entry_bits_cap_bytes);
    db->delta_entry_bits_cap_bytes = entry_bit_bytes ? entry_bit_bytes : 1u;
    db->entry_arrays_cap = db->header.entry_count;
    db->delta_entry_bits = (unsigned char*)calloc(db->delta_entry_bits_cap_bytes, 1);
    db->covered_base_bits = (unsigned char*)calloc(base_bit_bytes ? base_bit_bytes : 1u, 1);
    db->dirs = (EzdbDiskDir*)malloc((size_t)db->header.dir_records_raw_size);
    db->strings = (char*)malloc((size_t)db->header.strings_raw_size + 1u);
    db->file_index = (EzdbDiskIndex*)malloc(sizeof(EzdbDiskIndex) * (size_t)db->header.file_index_count);
    db->dir_index = (EzdbDiskIndex*)malloc(sizeof(EzdbDiskIndex) * (size_t)db->header.dir_index_count);
    db->entry_index = (EzdbDiskIndex*)malloc(sizeof(EzdbDiskIndex) * (size_t)db->header.entry_index_count);
    db->entry_detail_pages = (EzdbDiskPage*)malloc(sizeof(EzdbDiskPage) * (size_t)db->header.entry_detail_page_count);
    db->raw_blob_pages = (EzdbDiskPage*)malloc(sizeof(EzdbDiskPage) * (size_t)db->header.raw_blob_page_count);
    if ((!db->file_parent_dir_ids && db->header.base_file_count) ||
        (!db->file_name_offsets && db->header.base_file_count) ||
        (!db->file_name_lens && db->header.base_file_count) ||
        (!db->file_sizes32 && db->header.base_file_count) ||
        (!db->file_modified_times32 && db->header.base_file_count) ||
        (!db->archive_meta && db->header.base_file_count) ||
        (!db->entry_archive_ids && db->header.entry_count) ||
        (!db->entry_path_offsets && db->header.entry_count) ||
        (!db->entry_path_lens && db->header.entry_count) ||
        (!db->entry_next_in_archive && db->header.entry_count) ||
        (!db->archive_first_entry_ids && db->header.file_count) ||
        (!db->delta_entry_refs && db->header.entry_count) ||
        (!db->active_bits && db->header.file_count) ||
        (!db->active_entry_bits && db->header.entry_count) ||
        (!db->delta_entry_bits && db->header.entry_count) ||
        (!db->covered_base_bits && db->header.base_file_count) ||
        (!db->dirs && db->header.dir_records_raw_size) ||
        (!db->strings && db->header.strings_raw_size) || (!db->file_index && db->header.file_index_count) ||
        (!db->dir_index && db->header.dir_index_count) ||
        (!db->entry_index && db->header.entry_index_count) ||
        (!db->entry_detail_pages && db->header.entry_detail_page_count) ||
        (!db->raw_blob_pages && db->header.raw_blob_page_count)) {
        ezdb_close(db);
        return EZDB_ERR_MEMORY;
    }
    rc = read_file_records_compact_stream(fp,
                                          db->header.file_records_offset,
                                          db->header.file_records_size,
                                          db->header.file_records_flags,
                                          db,
                                          (uint32_t)db->header.base_file_count);
    if (rc == EZDB_OK) {
        rc = read_section_into(fp, db->header.dir_records_offset, db->header.dir_records_size, db->header.dir_records_raw_size, db->header.dir_records_flags, (unsigned char*)db->dirs);
    }
    if (rc == EZDB_OK) {
        rc = read_section_into(fp, db->header.strings_offset, db->header.strings_size, db->header.strings_raw_size, db->header.strings_flags, (unsigned char*)db->strings);
    }
    if (rc == EZDB_OK && db->header.archive_meta_offset && db->header.archive_meta_raw_size) {
        rc = read_section_into(fp, db->header.archive_meta_offset, db->header.archive_meta_size, db->header.archive_meta_raw_size, db->header.archive_meta_flags, (unsigned char*)db->archive_meta);
    }
    if (rc == EZDB_OK && db->header.entry_records_offset && db->header.entry_records_raw_size) {
        unsigned char* entry_core = NULL;
        rc = read_section_payload(fp, db->header.entry_records_offset, db->header.entry_records_size, db->header.entry_records_raw_size, db->header.entry_records_flags, &entry_core);
        uint64_t saved_entry_count = db->header.entry_count;
        db->header.entry_count = db->header.base_entry_count;
        if (rc == EZDB_OK) rc = decode_entry_core(db, entry_core, db->header.entry_records_raw_size);
        db->header.entry_count = saved_entry_count;
        free(entry_core);
    }
    if (rc == EZDB_OK && db->header.entry_detail_index_offset && db->header.entry_detail_page_count) {
        if (fseek(fp, (long)db->header.entry_detail_index_offset, SEEK_SET) != 0 ||
            fread(db->entry_detail_pages, sizeof(EzdbDiskPage), (size_t)db->header.entry_detail_page_count, fp) != (size_t)db->header.entry_detail_page_count) {
            rc = EZDB_ERR_IO;
        }
    }
    if (rc == EZDB_OK && db->header.raw_blob_index_offset && db->header.raw_blob_page_count) {
        if (fseek(fp, (long)db->header.raw_blob_index_offset, SEEK_SET) != 0 ||
            fread(db->raw_blob_pages, sizeof(EzdbDiskPage), (size_t)db->header.raw_blob_page_count, fp) != (size_t)db->header.raw_blob_page_count) {
            rc = EZDB_ERR_IO;
        }
    }
    if (rc != EZDB_OK) {
        ezdb_close(db);
        return rc;
    }
    if (fseek(fp, (long)db->header.file_index_offset, SEEK_SET) != 0 ||
        fread(db->file_index, sizeof(EzdbDiskIndex), (size_t)db->header.file_index_count, fp) != (size_t)db->header.file_index_count ||
        fseek(fp, (long)db->header.dir_index_offset, SEEK_SET) != 0 ||
        fread(db->dir_index, sizeof(EzdbDiskIndex), (size_t)db->header.dir_index_count, fp) != (size_t)db->header.dir_index_count) {
        ezdb_close(db);
        return EZDB_ERR_IO;
    }
    if (db->header.entry_index_count) {
        if (fseek(fp, (long)db->header.entry_index_offset, SEEK_SET) != 0 ||
            fread(db->entry_index, sizeof(EzdbDiskIndex), (size_t)db->header.entry_index_count, fp) != (size_t)db->header.entry_index_count) {
            ezdb_close(db);
            return EZDB_ERR_IO;
        }
        uint64_t max_entry_posting_end = 0;
        for (uint64_t i = 0; i < db->header.entry_index_count; ++i) {
            uint64_t end = db->entry_index[i].offset + db->entry_index[i].encoded_size;
            if (end > max_entry_posting_end) max_entry_posting_end = end;
        }
        if (max_entry_posting_end > db->header.postings_size) {
            db->header.entry_postings_size = max_entry_posting_end - db->header.postings_size;
        }
    }
    db->strings[db->header.strings_raw_size] = '\0';
    memset(db->active_bits, 0xff, logical_bit_bytes ? logical_bit_bytes : 1u);
    memset(db->active_entry_bits, 0, entry_bit_bytes ? entry_bit_bytes : 1u);
    size_t base_entry_bit_bytes = ((size_t)db->header.base_entry_count + 7u) / 8u;
    if (base_entry_bit_bytes) memset(db->active_entry_bits, 0xff, base_entry_bit_bytes);
    if (db->header.file_count & 7u) {
        db->active_bits[logical_bit_bytes - 1u] = (unsigned char)((1u << (db->header.file_count & 7u)) - 1u);
    }
    if (db->header.base_entry_count && (db->header.base_entry_count & 7u)) {
        db->active_entry_bits[base_entry_bit_bytes - 1u] =
            (unsigned char)((1u << (db->header.base_entry_count & 7u)) - 1u);
    }
    db->header.active_entry_count = db->header.base_entry_count;
    rebuild_archive_entry_links(db);
    rc = replay_delta_log(db);
    if (rc != EZDB_OK) {
        ezdb_close(db);
        return rc;
    }
    *out_db = db;
    return EZDB_OK;
}

static void ezdb_release_members(Ezdb* db, int free_path)
{
    if (!db) return;
    if (db->fp) fclose(db->fp);
    if (free_path) free(db->path);
    free(db->file_parent_dir_ids);
    free(db->file_name_offsets);
    free(db->file_name_lens);
    free(db->file_sizes32);
    free(db->file_size_overflow_ids);
    free(db->file_size_overflow_values);
    free(db->file_modified_times32);
    free(db->file_mtime_overflow_ids);
    free(db->file_mtime_overflow_values);
    free(db->archive_meta);
    free(db->entry_archive_ids);
    free(db->entry_path_offsets);
    free(db->entry_path_lens);
    free(db->entry_next_in_archive);
    free(db->archive_first_entry_ids);
    free(db->delta_entry_refs);
    free(db->entry_detail_pages);
    free(db->raw_blob_pages);
    ezdb_entries_page_cache_free(db->entry_detail_cache, EZDB_ENTRY_DETAIL_CACHE_PAGES);
    ezdb_entries_page_cache_free(db->raw_blob_cache, EZDB_RAW_BLOB_CACHE_PAGES);
    free(db->active_entry_bits);
    free(db->delta_entry_bits);
    free(db->dirs);
    free(db->strings);
    free(db->file_index);
    free(db->dir_index);
    free(db->entry_index);
    ezdb_postings_builder_free(&db->delta_entry_index);
    free(db->active_bits);
    free(db->covered_base_bits);
    free(db->txn_start_active_bits);
    free(db->txn_start_active_entry_bits);
    if (db->deltas) {
        for (uint32_t i = 0; i < db->delta_count; ++i) free(db->deltas[i].path);
    }
    free(db->deltas);
    free(db->delta_buckets);
}

void ezdb_close(Ezdb* db)
{
    if (!db) return;
    ezdb_release_members(db, 1);
    free(db);
}

uint32_t ezdb_count(Ezdb* db)
{
    return db ? (uint32_t)db->header.file_count : 0;
}

uint32_t ezdb_active_count(Ezdb* db)
{
    return db ? (uint32_t)db->header.active_count : 0;
}

uint32_t ezdb_archive_count(Ezdb* db)
{
    return ezdb_count(db);
}

uint32_t ezdb_active_archive_count(Ezdb* db)
{
    return ezdb_active_count(db);
}

uint32_t ezdb_entry_count(Ezdb* db)
{
    return db ? (uint32_t)db->header.entry_count : 0;
}

static uint32_t ezdb_compute_active_entry_count(Ezdb* db)
{
    if (!db) return 0;
    uint32_t count = 0;
    for (uint32_t i = 0; i < db->header.entry_count; ++i) {
        if (bitset_get(db->active_entry_bits, i) &&
            db->entry_archive_ids[i] < db->header.file_count &&
            bitset_get(db->active_bits, db->entry_archive_ids[i])) {
            ++count;
        }
    }
    return count;
}

uint32_t ezdb_active_entry_count(Ezdb* db)
{
    return ezdb_compute_active_entry_count(db);
}

uint64_t ezdb_file_size(Ezdb* db)
{
    return db && db->fp ? file_size_of(db->fp) : 0;
}

int ezdb_stats(Ezdb* db, EzdbStats* out_stats)
{
    if (!db || !out_stats) return EZDB_ERR_ARG;
    memset(out_stats, 0, sizeof(*out_stats));
    out_stats->record_count = (uint32_t)db->header.file_count;
    out_stats->active_count = (uint32_t)db->header.active_count;
    out_stats->entry_count = (uint32_t)db->header.entry_count;
    out_stats->active_entry_count = ezdb_compute_active_entry_count(db);
    out_stats->base_entry_count = (uint32_t)db->header.base_entry_count;
    out_stats->delta_entry_count = (uint32_t)(db->header.entry_count - db->header.base_entry_count);
    out_stats->file_size = ezdb_file_size(db);
    out_stats->delta_size = db->header.delta_size;
    out_stats->records_size = db->header.file_records_size;
    out_stats->dirs_size = db->header.dir_records_size;
    out_stats->names_size = db->header.strings_size;
    out_stats->archive_meta_size = db->header.archive_meta_size;
    out_stats->entry_records_size = db->header.entry_records_size;
    out_stats->raw_blob_size = db->header.raw_blob_size;
    out_stats->index_size = db->header.file_index_count * sizeof(EzdbDiskIndex) +
                            db->header.dir_index_count * sizeof(EzdbDiskIndex) +
                            db->header.entry_index_count * sizeof(EzdbDiskIndex);
    out_stats->postings_size = db->header.postings_size + db->header.entry_postings_size;
    return EZDB_OK;
}

int ezdb_get_by_id(Ezdb* db, uint32_t id, EzdbSearchResult* out_result)
{
    return build_result_path(db, id, out_result);
}

void ezdb_free_result(EzdbSearchResult* result)
{
    if (!result) return;
    free(result->path);
    memset(result, 0, sizeof(*result));
}

int ezdb_get_archive(Ezdb* db, uint32_t id, EzdbArchiveResult* out_result)
{
    if (!db || !out_result) return EZDB_ERR_ARG;
    EzdbSearchResult path_result;
    int rc = build_result_path(db, id, &path_result);
    if (rc != EZDB_OK) return rc;
    memset(out_result, 0, sizeof(*out_result));
    out_result->id = id;
    out_result->file_path = path_result.path;
    out_result->file_size = path_result.size;
    out_result->modified_time = path_result.modified_time;
    if (id < db->header.base_file_count && db->archive_meta) {
        out_result->drive_letter = (char)db->archive_meta[id].drive_letter;
        out_result->file_ref_number = db->archive_meta[id].file_ref_number;
        out_result->usn = db->archive_meta[id].usn;
    }
    return EZDB_OK;
}

static int mark_entry_scope_literal_candidates(Ezdb* db, const char* literal, size_t literal_len, uint32_t scope, unsigned char* seen, int* any_marked)
{
    int rc = EZDB_OK;
    if (scope & EZDB_SEARCH_ENTRY_PATH) {
        rc = mark_entry_literal_candidates(db, literal, literal_len, seen, any_marked);
    }
    if (rc == EZDB_OK && (scope & EZDB_SEARCH_COMBINED_PATH)) {
        rc = mark_entry_literal_candidates(db, literal, literal_len, seen, any_marked);
        if (rc == EZDB_OK) rc = mark_archive_literal_entry_candidates(db, literal, literal_len, seen, any_marked);
    }
    return rc;
}

static int query_build_entry_candidate_bitset(Ezdb* db, EzdbQueryNode* node, const char* fallback_keyword, uint32_t scope, unsigned char** out_bits, int* out_has_positive)
{
    *out_bits = NULL;
    *out_has_positive = 0;
    size_t bit_bytes = ((size_t)db->header.entry_count + 7u) / 8u;
    if (!node) {
        unsigned char* bits = (unsigned char*)calloc(bit_bytes ? bit_bytes : 1u, 1);
        if (!bits) return EZDB_ERR_MEMORY;
        int any_marked = 0;
        int rc = mark_entry_scope_literal_candidates(db, fallback_keyword, strlen(fallback_keyword), scope, bits, &any_marked);
        if (rc != EZDB_OK) {
            free(bits);
            return rc;
        }
        *out_bits = bits;
        *out_has_positive = 1;
        return EZDB_OK;
    }
    if (node->type == EZDB_QUERY_NOT) return EZDB_OK;
    if (node->type == EZDB_QUERY_TERM || node->type == EZDB_QUERY_WILDCARD) {
        const char* literal = NULL;
        size_t literal_len = 0;
        if (node->type == EZDB_QUERY_TERM) {
            literal = node->text;
            literal_len = node->text_len;
        } else if (!ezdb_query_longest_literal_from_wildcard(node->text, node->text_len, &literal, &literal_len)) {
            return EZDB_OK;
        }
        unsigned char* bits = (unsigned char*)calloc(bit_bytes ? bit_bytes : 1u, 1);
        if (!bits) return EZDB_ERR_MEMORY;
        int any_marked = 0;
        int rc = mark_entry_scope_literal_candidates(db, literal, literal_len, scope, bits, &any_marked);
        if (rc != EZDB_OK) {
            free(bits);
            return rc;
        }
        *out_bits = bits;
        *out_has_positive = 1;
        return EZDB_OK;
    }
    if (node->type == EZDB_QUERY_AND || node->type == EZDB_QUERY_OR) {
        unsigned char* left = NULL;
        unsigned char* right = NULL;
        int left_positive = 0;
        int right_positive = 0;
        int rc = query_build_entry_candidate_bitset(db, node->left, fallback_keyword, scope, &left, &left_positive);
        if (rc == EZDB_OK) rc = query_build_entry_candidate_bitset(db, node->right, fallback_keyword, scope, &right, &right_positive);
        if (rc != EZDB_OK) {
            free(left);
            free(right);
            return rc;
        }
        if (node->type == EZDB_QUERY_AND) {
            if (left_positive && right_positive) {
                bitset_and_into(left, right, bit_bytes);
                free(right);
                *out_bits = left;
                *out_has_positive = 1;
            } else if (left_positive) {
                *out_bits = left;
                *out_has_positive = 1;
                free(right);
            } else if (right_positive) {
                *out_bits = right;
                *out_has_positive = 1;
                free(left);
            } else {
                free(left);
                free(right);
            }
        } else {
            if (left_positive && right_positive) {
                bitset_or_into(left, right, bit_bytes);
                free(right);
                *out_bits = left;
                *out_has_positive = 1;
            } else {
                free(left);
                free(right);
                *out_bits = NULL;
                *out_has_positive = 0;
            }
        }
    }
    return EZDB_OK;
}

void ezdb_free_entry_result(EzdbEntryResult* result);

int ezdb_get_entry(Ezdb* db, uint32_t id, EzdbEntryResult* out_result)
{
    if (!db || !out_result) return EZDB_ERR_ARG;
    if (id >= db->header.entry_count || !bitset_get(db->active_entry_bits, id)) return EZDB_ERR_NOT_FOUND;
    EzdbDiskEntry detail;
    EzdbEntryDetailStore store = entry_detail_store(db);
    int rc = ezdb_entries_load_detail(&store, id, &detail);
    if (rc != EZDB_OK) return rc;
    if (detail.archive_id >= db->header.file_count || !bitset_get(db->active_bits, detail.archive_id)) return EZDB_ERR_NOT_FOUND;
    EzdbEntryPathStore path_store = entry_path_store(db);
    char* entry_path = ezdb_entries_copy_path(&path_store, id);
    if (!entry_path) return EZDB_ERR_MEMORY;

    memset(out_result, 0, sizeof(*out_result));
    out_result->id = id;
    out_result->archive_id = detail.archive_id;
    out_result->entry_path = entry_path;
    EzdbSearchResult archive;
    rc = build_result_path(db, detail.archive_id, &archive);
    if (rc != EZDB_OK) {
        ezdb_free_entry_result(out_result);
        return rc;
    }
    out_result->archive_path = archive.path;
    out_result->compressed_size = detail.compressed_size;
    out_result->original_size = detail.original_size;
    out_result->modified_time = detail.modified_time;
    if (detail.raw_len) {
        out_result->entry_raw_path = ezdb_entries_copy_raw_path(&path_store, id, &detail);
        if (!out_result->entry_raw_path) {
            ezdb_free_entry_result(out_result);
            return EZDB_ERR_MEMORY;
        }
        out_result->entry_raw_path_len = detail.raw_len;
    }
    return EZDB_OK;
}

void ezdb_free_archive_result(EzdbArchiveResult* result)
{
    if (!result) return;
    free(result->file_path);
    memset(result, 0, sizeof(*result));
}

void ezdb_free_entry_result(EzdbEntryResult* result)
{
    if (!result) return;
    free(result->archive_path);
    free(result->entry_path);
    free(result->entry_raw_path);
    memset(result, 0, sizeof(*result));
}

void ezdb_free_search_v2_result(EzdbSearchV2Result* result)
{
    if (!result) return;
    free(result->archive_path);
    free(result->entry_path);
    free(result->entry_raw_path);
    memset(result, 0, sizeof(*result));
}

static int ezdb_search_plain(Ezdb* db, const char* keyword, uint32_t limit, EzdbSearchCallback callback, void* user_data)
{
    size_t key_len = strlen(keyword);
    if (!key_len) return EZDB_OK;
    uint32_t* keys = NULL;
    uint32_t key_count = 0;
    int rc = ezdb_postings_build_query_keys(keyword, &keys, &key_count);
    if (rc != EZDB_OK) return rc;

    uint32_t* file_ids = NULL;
    uint32_t file_count = 0;
    uint32_t* dir_ids = NULL;
    uint32_t dir_count = 0;
    rc = ezdb_postings_load_intersected(db->fp, db->header.postings_offset, db->file_index, db->header.file_index_count, keys, key_count, &file_ids, &file_count);
    if (rc == EZDB_OK) rc = ezdb_postings_load_intersected(db->fp, db->header.postings_offset, db->dir_index, db->header.dir_index_count, keys, key_count, &dir_ids, &dir_count);
    free(keys);
    if (rc != EZDB_OK) {
        free(file_ids);
        free(dir_ids);
        return rc;
    }

    size_t seen_size = ((size_t)db->header.file_count + 7u) / 8u;
    unsigned char* seen = (unsigned char*)calloc(seen_size ? seen_size : 1u, 1);
    if (!seen && db->header.file_count) {
        free(file_ids);
        free(dir_ids);
        return EZDB_ERR_MEMORY;
    }
    for (uint32_t i = 0; i < file_count; ++i) {
        if (file_ids[i] < db->header.base_file_count &&
            bitset_get(db->active_bits, file_ids[i]) &&
            !bitset_get(db->covered_base_bits, file_ids[i])) {
            seen[file_ids[i] >> 3u] |= (unsigned char)(1u << (file_ids[i] & 7u));
        }
    }
    for (uint32_t i = 0; i < dir_count; ++i) {
        uint32_t dir_id = dir_ids[i];
        if (dir_id >= db->header.dir_count) continue;
        EzdbDiskDir* d = &db->dirs[dir_id];
        uint32_t end = d->first_file_id + d->file_count;
        if (end > db->header.base_file_count) end = (uint32_t)db->header.base_file_count;
        for (uint32_t id = d->first_file_id; id < end; ++id) {
            if (bitset_get(db->active_bits, id) && !bitset_get(db->covered_base_bits, id)) {
                seen[id >> 3u] |= (unsigned char)(1u << (id & 7u));
            }
        }
    }
    for (uint32_t i = 0; i < db->delta_count; ++i) {
        EzdbDeltaRecord* delta = &db->deltas[i];
        if (find_delta_record(db, delta->id) != delta) continue;
        if (delta->type == EZDB_DELTA_DELETE || !bitset_get(db->active_bits, delta->id)) continue;
        if (ezdb_query_contains_ascii_casefold(delta->path, delta->path_len, keyword, key_len)) {
            seen[delta->id >> 3u] |= (unsigned char)(1u << (delta->id & 7u));
        }
    }

    uint32_t emitted = 0;
    for (uint32_t id = 0; id < db->header.file_count; ++id) {
        if (limit && emitted >= limit) break;
        if (!(seen[id >> 3u] & (unsigned char)(1u << (id & 7u)))) continue;
        if (key_len > EZDB_GRAM3 && !record_contains_keyword(db, id, keyword, key_len)) continue;
        EzdbSearchResult result;
        rc = build_result_path(db, id, &result);
        if (rc == EZDB_ERR_NOT_FOUND) {
            rc = EZDB_OK;
            continue;
        }
        if (rc != EZDB_OK) break;
        callback(&result, user_data);
        ++emitted;
        ezdb_free_result(&result);
    }
    free(seen);
    free(file_ids);
    free(dir_ids);
    return rc;
}

int ezdb_search_path(Ezdb* db, const char* keyword, uint32_t limit, EzdbSearchCallback callback, void* user_data)
{
    if (!db || !keyword || !callback) return EZDB_ERR_ARG;
    while (ezdb_query_is_space((unsigned char)*keyword)) ++keyword;
    if (!*keyword) return EZDB_OK;

    EzdbQueryNode* root = ezdb_query_parse(keyword);
    if (!root) return ezdb_search_plain(db, keyword, limit, callback, user_data);

    unsigned char* seen = NULL;
    int has_positive_candidates = 0;
    int rc = query_build_candidate_bitset(db, root, &seen, &has_positive_candidates);
    if (rc != EZDB_OK) {
        ezdb_query_node_free(root);
        return rc;
    }
    size_t seen_size = ((size_t)db->header.file_count + 7u) / 8u;
    int full_scan = !has_positive_candidates;
    if (!full_scan && !bitset_any(seen, seen_size)) {
        free(seen);
        ezdb_query_node_free(root);
        return EZDB_OK;
    }

    uint32_t emitted = 0;
    for (uint32_t id = 0; rc == EZDB_OK && id < db->header.file_count; ++id) {
        if (limit && emitted >= limit) break;
        if (full_scan) {
            if (!bitset_get(db->active_bits, id)) continue;
        } else if (!(seen[id >> 3u] & (unsigned char)(1u << (id & 7u)))) {
            continue;
        }
        EzdbSearchResult result;
        rc = build_result_path(db, id, &result);
        if (rc == EZDB_ERR_NOT_FOUND) {
            rc = EZDB_OK;
            continue;
        }
        if (rc != EZDB_OK) break;
        if (ezdb_query_match_path(root, result.path, strlen(result.path))) {
            callback(&result, user_data);
            ++emitted;
        }
        ezdb_free_result(&result);
    }

    free(seen);
    ezdb_query_node_free(root);
    return rc;
}

typedef struct EzdbArchiveSearchAdapter {
    Ezdb* db;
    EzdbSearchV2Callback callback;
    void* user_data;
    uint32_t emitted;
} EzdbArchiveSearchAdapter;

static void ezdb_archive_search_adapter_cb(const EzdbSearchResult* result, void* user_data)
{
    EzdbArchiveSearchAdapter* adapter = (EzdbArchiveSearchAdapter*)user_data;
    EzdbSearchV2Result out;
    memset(&out, 0, sizeof(out));
    out.kind = EZDB_RESULT_ARCHIVE;
    out.id = result->id;
    out.archive_id = result->id;
    out.archive_path = ezdb_strdup_range(result->path, strlen(result->path));
    out.file_size = result->size;
    out.modified_time = result->modified_time;
    if (adapter->db && result->id < adapter->db->header.base_file_count && adapter->db->archive_meta) {
        EzdbDiskArchiveMeta* meta = &adapter->db->archive_meta[result->id];
        out.drive_letter = (char)meta->drive_letter;
        out.file_ref_number = meta->file_ref_number;
        out.usn = meta->usn;
    }
    adapter->callback(&out, adapter->user_data);
    adapter->emitted += 1u;
    ezdb_free_search_v2_result(&out);
}

static int ezdb_emit_entry_result(Ezdb* db, uint32_t id, EzdbSearchV2Callback callback, void* user_data)
{
    EzdbEntryResult entry;
    int rc = ezdb_get_entry(db, id, &entry);
    if (rc != EZDB_OK) return rc;
    EzdbSearchV2Result out;
    memset(&out, 0, sizeof(out));
    out.kind = EZDB_RESULT_ENTRY;
    out.id = id;
    out.archive_id = entry.archive_id;
    out.archive_path = entry.archive_path;
    out.entry_path = entry.entry_path;
    out.entry_raw_path = entry.entry_raw_path;
    out.entry_raw_path_len = entry.entry_raw_path_len;
    out.compressed_size = entry.compressed_size;
    out.original_size = entry.original_size;
    out.modified_time = entry.modified_time;
    entry.archive_path = NULL;
    entry.entry_path = NULL;
    entry.entry_raw_path = NULL;
    callback(&out, user_data);
    ezdb_free_search_v2_result(&out);
    memset(&entry, 0, sizeof(entry));
    return EZDB_OK;
}

static int ezdb_emit_entry_result_with_path(Ezdb* db, uint32_t id, char* entry_path, EzdbSearchV2Callback callback, void* user_data)
{
    if (!entry_path) return ezdb_emit_entry_result(db, id, callback, user_data);
    EzdbDiskEntry detail;
    EzdbEntryDetailStore store = entry_detail_store(db);
    int rc = ezdb_entries_load_detail(&store, id, &detail);
    if (rc != EZDB_OK) return rc;
    EzdbSearchResult archive;
    rc = build_result_path(db, detail.archive_id, &archive);
    if (rc != EZDB_OK) return rc;
    EzdbSearchV2Result out;
    memset(&out, 0, sizeof(out));
    out.kind = EZDB_RESULT_ENTRY;
    out.id = id;
    out.archive_id = detail.archive_id;
    out.archive_path = archive.path;
    out.entry_path = entry_path;
    out.compressed_size = detail.compressed_size;
    out.original_size = detail.original_size;
    out.modified_time = detail.modified_time;
    if (detail.raw_len) {
        EzdbEntryPathStore path_store = entry_path_store(db);
        out.entry_raw_path = ezdb_entries_copy_raw_path(&path_store, id, &detail);
        if (!out.entry_raw_path) {
            archive.path = NULL;
            ezdb_free_search_v2_result(&out);
            return EZDB_ERR_MEMORY;
        }
        out.entry_raw_path_len = detail.raw_len;
    }
    archive.path = NULL;
    callback(&out, user_data);
    ezdb_free_search_v2_result(&out);
    return EZDB_OK;
}

int ezdb_search(Ezdb* db, const char* keyword, uint32_t scope, uint32_t limit, EzdbSearchV2Callback callback, void* user_data)
{
    if (!db || !keyword || !callback) return EZDB_ERR_ARG;
    while (ezdb_query_is_space((unsigned char)*keyword)) ++keyword;
    if (!*keyword) return EZDB_OK;
    if (!scope) scope = EZDB_SEARCH_ARCHIVE_PATH;

    uint32_t emitted = 0;
    int rc = EZDB_OK;
    if (scope & EZDB_SEARCH_ARCHIVE_PATH) {
        EzdbArchiveSearchAdapter adapter;
        adapter.db = db;
        adapter.callback = callback;
        adapter.user_data = user_data;
        adapter.emitted = 0;
        rc = ezdb_search_path(db, keyword, limit, ezdb_archive_search_adapter_cb, &adapter);
        if (rc != EZDB_OK) return rc;
        emitted = adapter.emitted;
        if (limit && emitted >= limit) {
            return EZDB_OK;
        }
    }

    if (!(scope & (EZDB_SEARCH_ENTRY_PATH | EZDB_SEARCH_COMBINED_PATH))) return EZDB_OK;

    EzdbQueryNode* root = ezdb_query_parse(keyword);
    unsigned char* entry_seen = NULL;
    int has_entry_candidates = 0;
    rc = query_build_entry_candidate_bitset(db, root, keyword, scope, &entry_seen, &has_entry_candidates);
    if (rc != EZDB_OK) {
        ezdb_query_node_free(root);
        return rc;
    }
    size_t entry_seen_size = ((size_t)db->header.entry_count + 7u) / 8u;
    int full_entry_scan = !has_entry_candidates;
    if (!full_entry_scan && !bitset_any(entry_seen, entry_seen_size)) {
        free(entry_seen);
        ezdb_query_node_free(root);
        return EZDB_OK;
    }
    EzdbEntryPathStore path_store = entry_path_store(db);
    for (uint32_t id = 0; rc == EZDB_OK && id < db->header.entry_count; ++id) {
        if (limit && emitted >= limit) break;
        if (full_entry_scan) {
            if (!bitset_get(db->active_entry_bits, id)) continue;
        } else if (!(entry_seen[id >> 3u] & (unsigned char)(1u << (id & 7u)))) {
            continue;
        }
        uint32_t archive_id = db->entry_archive_ids[id];
        if (archive_id >= db->header.file_count || !bitset_get(db->active_bits, archive_id)) continue;
        char* entry_path = ezdb_entries_copy_path(&path_store, id);
        if (!entry_path) continue;
        uint32_t entry_path_len = db->entry_path_lens[id];
        int matched = 0;
        if (scope & EZDB_SEARCH_ENTRY_PATH) {
            matched = ezdb_query_matches_text(root, keyword, entry_path, entry_path_len);
        }
        if (!matched && (scope & EZDB_SEARCH_COMBINED_PATH)) {
            EzdbSearchResult archive;
            rc = build_result_path(db, archive_id, &archive);
            if (rc == EZDB_ERR_NOT_FOUND) {
                rc = EZDB_OK;
                free(entry_path);
                continue;
            }
            if (rc != EZDB_OK) {
                free(entry_path);
                break;
            }
            size_t archive_len = strlen(archive.path);
            size_t combo_len = archive_len + 1u + entry_path_len;
            char* combo = (char*)malloc(combo_len + 1u);
            if (!combo) {
                ezdb_free_result(&archive);
                free(entry_path);
                rc = EZDB_ERR_MEMORY;
                break;
            }
            memcpy(combo, archive.path, archive_len);
            combo[archive_len] = '\n';
            memcpy(combo + archive_len + 1u, entry_path, entry_path_len);
            combo[combo_len] = '\0';
            matched = ezdb_query_matches_text(root, keyword, combo, combo_len);
            free(combo);
            ezdb_free_result(&archive);
        }
        if (matched) {
            rc = ezdb_emit_entry_result_with_path(db, id, entry_path, callback, user_data);
            if (rc == EZDB_OK) {
                entry_path = NULL;
                ++emitted;
            }
        }
        free(entry_path);
    }
    free(entry_seen);
    ezdb_query_node_free(root);
    return rc;
}

int ezdb_get_archive_by_ref(Ezdb* db, char drive_letter, uint64_t file_ref_number, EzdbArchiveResult* out_result)
{
    if (!db || !out_result) return EZDB_ERR_ARG;
    for (uint32_t i = 0; i < db->header.base_file_count; ++i) {
        if (bitset_get(db->active_bits, i) &&
            db->archive_meta &&
            db->archive_meta[i].drive_letter == (unsigned char)drive_letter &&
            db->archive_meta[i].file_ref_number == file_ref_number) {
            return ezdb_get_archive(db, i, out_result);
        }
    }
    return EZDB_ERR_NOT_FOUND;
}

typedef struct EzdbIdVec {
    uint32_t* ids;
    uint32_t count;
    uint32_t cap;
} EzdbIdVec;

static int ezdb_id_vec_push(EzdbIdVec* vec, uint32_t id)
{
    if (vec->count == vec->cap) {
        uint32_t next = vec->cap ? vec->cap * 2u : 1024u;
        uint32_t* ids = (uint32_t*)realloc(vec->ids, sizeof(uint32_t) * (size_t)next);
        if (!ids) return EZDB_ERR_MEMORY;
        vec->ids = ids;
        vec->cap = next;
    }
    vec->ids[vec->count++] = id;
    return EZDB_OK;
}

static int ezdb_entry_matches_query_scope(Ezdb* db,
                                          EzdbQueryNode* root,
                                          const char* keyword,
                                          uint32_t scope,
                                          uint32_t archive_id,
                                          const char* entry_path,
                                          uint32_t entry_path_len,
                                          int* out_matched)
{
    *out_matched = 0;
    if (scope & EZDB_SEARCH_ENTRY_PATH) {
        if (ezdb_query_matches_text(root, keyword, entry_path, entry_path_len)) {
            *out_matched = 1;
            return EZDB_OK;
        }
    }
    if (!(scope & (EZDB_SEARCH_ARCHIVE_PATH | EZDB_SEARCH_COMBINED_PATH))) return EZDB_OK;

    EzdbSearchResult archive;
    int rc = build_result_path(db, archive_id, &archive);
    if (rc == EZDB_ERR_NOT_FOUND) return EZDB_OK;
    if (rc != EZDB_OK) return rc;

    if (scope & EZDB_SEARCH_ARCHIVE_PATH) {
        *out_matched = ezdb_query_matches_text(root, keyword, archive.path, strlen(archive.path));
    }
    if (!*out_matched && (scope & EZDB_SEARCH_COMBINED_PATH)) {
        size_t archive_len = strlen(archive.path);
        size_t combo_len = archive_len + 1u + entry_path_len;
        char* combo = (char*)malloc(combo_len + 1u);
        if (!combo) {
            ezdb_free_result(&archive);
            return EZDB_ERR_MEMORY;
        }
        memcpy(combo, archive.path, archive_len);
        combo[archive_len] = '\n';
        memcpy(combo + archive_len + 1u, entry_path, entry_path_len);
        combo[combo_len] = '\0';
        *out_matched = ezdb_query_matches_text(root, keyword, combo, combo_len);
        free(combo);
    }
    ezdb_free_result(&archive);
    return EZDB_OK;
}

static int ezdb_collect_matching_entry_ids(Ezdb* db, const char* keyword, uint32_t scope, EzdbIdVec* vec)
{
    if (!(scope & (EZDB_SEARCH_ARCHIVE_PATH | EZDB_SEARCH_ENTRY_PATH | EZDB_SEARCH_COMBINED_PATH))) return EZDB_OK;

    EzdbQueryNode* root = ezdb_query_parse(keyword);
    unsigned char* entry_seen = NULL;
    int has_entry_candidates = 0;
    uint32_t candidate_scope = scope;
    if (scope & EZDB_SEARCH_ARCHIVE_PATH) candidate_scope |= EZDB_SEARCH_COMBINED_PATH;
    int rc = query_build_entry_candidate_bitset(db, root, keyword, candidate_scope, &entry_seen, &has_entry_candidates);
    if (rc != EZDB_OK) {
        ezdb_query_node_free(root);
        return rc;
    }
    size_t entry_seen_size = ((size_t)db->header.entry_count + 7u) / 8u;
    int full_entry_scan = !has_entry_candidates;
    if (!full_entry_scan && !bitset_any(entry_seen, entry_seen_size)) {
        free(entry_seen);
        ezdb_query_node_free(root);
        return EZDB_OK;
    }

    EzdbEntryPathStore path_store = entry_path_store(db);
    for (uint32_t id = 0; rc == EZDB_OK && id < db->header.entry_count; ++id) {
        if (full_entry_scan) {
            if (!bitset_get(db->active_entry_bits, id)) continue;
        } else if (!(entry_seen[id >> 3u] & (unsigned char)(1u << (id & 7u)))) {
            continue;
        }
        uint32_t archive_id = db->entry_archive_ids[id];
        if (archive_id >= db->header.file_count || !bitset_get(db->active_bits, archive_id)) continue;
        char* entry_path = ezdb_entries_copy_path(&path_store, id);
        if (!entry_path) continue;
        int matched = 0;
        rc = ezdb_entry_matches_query_scope(db,
                                            root,
                                            keyword,
                                            scope,
                                            archive_id,
                                            entry_path,
                                            db->entry_path_lens[id],
                                            &matched);
        free(entry_path);
        if (rc == EZDB_OK && matched) rc = ezdb_id_vec_push(vec, id);
    }
    free(entry_seen);
    ezdb_query_node_free(root);
    return rc;
}

static const char* ezdb_basename(const char* path)
{
    const char* name = path;
    for (const char* p = path; p && *p; ++p) {
        if (*p == '/' || *p == '\\') name = p + 1;
    }
    return name ? name : "";
}

static char* ezdb_entry_sort_string(Ezdb* db, uint32_t id, int sort_column)
{
    EzdbEntryResult entry;
    if (ezdb_get_entry(db, id, &entry) != EZDB_OK) return ezdb_strdup_range("", 0);
    const char* src = "";
    if (sort_column == 0) {
        src = ezdb_basename(entry.entry_path ? entry.entry_path : "");
    } else if (sort_column == 1) {
        src = entry.archive_path ? entry.archive_path : "";
    } else if (sort_column == 2) {
        src = entry.entry_path ? entry.entry_path : "";
    }
    char* out = ezdb_strdup_range(src, strlen(src));
    ezdb_free_entry_result(&entry);
    return out;
}

static int ezdb_compare_entry_ids(Ezdb* db, uint32_t lhs, uint32_t rhs, int sort_column, int sort_ascending)
{
    int cmp = 0;
    if (sort_column == 0 || sort_column == 1 || sort_column == 2) {
        char* a = ezdb_entry_sort_string(db, lhs, sort_column);
        char* b = ezdb_entry_sort_string(db, rhs, sort_column);
        if (!a || !b) {
            free(a);
            free(b);
            cmp = 0;
        } else {
            cmp = strcmp(a, b);
            free(a);
            free(b);
        }
    } else if (sort_column == 3 || sort_column == 4 || sort_column == 5) {
        EzdbDiskEntry ad, bd;
        EzdbEntryDetailStore store = entry_detail_store(db);
        if (ezdb_entries_load_detail(&store, lhs, &ad) != EZDB_OK ||
            ezdb_entries_load_detail(&store, rhs, &bd) != EZDB_OK) {
            cmp = 0;
        } else if (sort_column == 3) {
            int a_missing = ad.compressed_size < 0;
            int b_missing = bd.compressed_size < 0;
            if (a_missing != b_missing) cmp = a_missing ? 1 : -1;
            else if (ad.compressed_size < bd.compressed_size) cmp = -1;
            else if (ad.compressed_size > bd.compressed_size) cmp = 1;
        } else if (sort_column == 4) {
            if (ad.original_size < bd.original_size) cmp = -1;
            else if (ad.original_size > bd.original_size) cmp = 1;
        } else {
            if (ad.modified_time < bd.modified_time) cmp = -1;
            else if (ad.modified_time > bd.modified_time) cmp = 1;
        }
    } else {
        if (lhs < rhs) cmp = -1;
        else if (lhs > rhs) cmp = 1;
    }
    if (cmp == 0) {
        if (lhs < rhs) cmp = -1;
        else if (lhs > rhs) cmp = 1;
    }
    return sort_ascending ? cmp : -cmp;
}

static void ezdb_sort_entry_ids(Ezdb* db, uint32_t* ids, uint32_t count, int sort_column, int sort_ascending)
{
    if (!ids || count < 2) return;
    for (uint32_t i = 1; i < count; ++i) {
        uint32_t value = ids[i];
        uint32_t j = i;
        while (j > 0 && ezdb_compare_entry_ids(db, ids[j - 1u], value, sort_column, sort_ascending) > 0) {
            ids[j] = ids[j - 1u];
            --j;
        }
        ids[j] = value;
    }
}

int ezdb_query_entries(Ezdb* db, const EzdbEntryQuery* query, EzdbEntryQueryPage* out_page)
{
    if (!db || !query || !out_page) return EZDB_ERR_ARG;
    memset(out_page, 0, sizeof(*out_page));

    EzdbIdVec vec;
    memset(&vec, 0, sizeof(vec));
    const char* keyword = query->keyword ? query->keyword : "";
    while (ezdb_query_is_space((unsigned char)*keyword)) ++keyword;

    int rc = EZDB_OK;
    if (!*keyword) {
        for (uint32_t i = 0; i < db->header.entry_count; ++i) {
            if (entry_is_searchable(db, i)) {
                rc = ezdb_id_vec_push(&vec, i);
                if (rc != EZDB_OK) break;
            }
        }
    } else {
        uint32_t scope = query->scope ? query->scope : EZDB_SEARCH_COMBINED_PATH;
        rc = ezdb_collect_matching_entry_ids(db, keyword, scope, &vec);
    }
    if (rc != EZDB_OK) {
        free(vec.ids);
        return rc;
    }

    if (query->sort_column >= 0 || !query->sort_ascending) {
        ezdb_sort_entry_ids(db, vec.ids, vec.count, query->sort_column, query->sort_ascending != 0);
    }

    out_page->total_count = vec.count;
    uint32_t offset = query->offset;
    uint32_t available = offset < vec.count ? vec.count - offset : 0;
    uint32_t wanted = query->limit ? query->limit : available;
    if (wanted > available) wanted = available;
    if (wanted) {
        out_page->ids = (uint32_t*)malloc(sizeof(uint32_t) * (size_t)wanted);
        if (!out_page->ids) {
            free(vec.ids);
            memset(out_page, 0, sizeof(*out_page));
            return EZDB_ERR_MEMORY;
        }
        memcpy(out_page->ids, vec.ids + offset, sizeof(uint32_t) * (size_t)wanted);
        out_page->returned_count = wanted;
    }
    free(vec.ids);
    return EZDB_OK;
}

int ezdb_get_entries_batch(Ezdb* db, const uint32_t* ids, uint32_t count, EzdbEntryResult* out_results)
{
    if (!db || (!ids && count) || (!out_results && count)) return EZDB_ERR_ARG;
    for (uint32_t i = 0; i < count; ++i) memset(&out_results[i], 0, sizeof(out_results[i]));
    for (uint32_t i = 0; i < count; ++i) {
        int rc = ezdb_get_entry(db, ids[i], &out_results[i]);
        if (rc != EZDB_OK) {
            for (uint32_t j = 0; j < i; ++j) ezdb_free_entry_result(&out_results[j]);
            return rc;
        }
    }
    return EZDB_OK;
}

void ezdb_free_entry_query_page(EzdbEntryQueryPage* page)
{
    if (!page) return;
    free(page->ids);
    memset(page, 0, sizeof(*page));
}

int ezdb_begin_write(Ezdb* db, uint32_t flags)
{
    (void)flags;
    if (!db) return EZDB_ERR_ARG;
    if (db->read_only) return EZDB_ERR_READ_ONLY;
    if (db->write_txn_active) return EZDB_ERR_ARG;
    db->txn_start_header = db->header;
    db->txn_start_delta_count = db->delta_count;
    db->txn_start_delta_cap = db->delta_cap;
    db->txn_start_active_bit_bytes = ((size_t)db->header.file_count + 7u) / 8u;
    db->txn_start_active_entry_bit_bytes = ((size_t)db->header.entry_count + 7u) / 8u;
    db->txn_start_entry_count = db->header.entry_count;
    db->txn_start_active_entry_count = db->header.active_entry_count;
    free(db->txn_start_active_bits);
    free(db->txn_start_active_entry_bits);
    db->txn_start_active_entry_bits = NULL;
    db->txn_start_active_bits = (unsigned char*)malloc(db->txn_start_active_bit_bytes ? db->txn_start_active_bit_bytes : 1u);
    if (!db->txn_start_active_bits) return EZDB_ERR_MEMORY;
    db->txn_start_active_entry_bits = (unsigned char*)malloc(db->txn_start_active_entry_bit_bytes ? db->txn_start_active_entry_bit_bytes : 1u);
    if (!db->txn_start_active_entry_bits) {
        free(db->txn_start_active_bits);
        db->txn_start_active_bits = NULL;
        return EZDB_ERR_MEMORY;
    }
    memcpy(db->txn_start_active_bits, db->active_bits, db->txn_start_active_bit_bytes ? db->txn_start_active_bit_bytes : 1u);
    memcpy(db->txn_start_active_entry_bits, db->active_entry_bits, db->txn_start_active_entry_bit_bytes ? db->txn_start_active_entry_bit_bytes : 1u);
    db->write_txn_active = 1;
    db->batch_index_deferred = 1;
    db->batch_index_dirty = 0;
    int rc = append_delta_frame(db, EZDB_DELTA_BATCH_BEGIN);
    if (rc != EZDB_OK) {
        db->write_txn_active = 0;
        free(db->txn_start_active_bits);
        db->txn_start_active_bits = NULL;
        free(db->txn_start_active_entry_bits);
        db->txn_start_active_entry_bits = NULL;
        db->txn_start_active_bit_bytes = 0;
        db->txn_start_active_entry_bit_bytes = 0;
        return rc;
    }
    return EZDB_OK;
}

int ezdb_commit_write(Ezdb* db)
{
    if (!db) return EZDB_ERR_ARG;
    if (db->read_only) return EZDB_ERR_READ_ONLY;
    if (!db->write_txn_active) return EZDB_ERR_ARG;
    int rc = append_delta_frame(db, EZDB_DELTA_BATCH_COMMIT);
    if (rc == EZDB_OK) rc = write_header(db);
    if (rc != EZDB_OK) {
        (void)restore_txn_snapshot(db);
        db->write_txn_active = 0;
        free(db->txn_start_active_bits);
        db->txn_start_active_bits = NULL;
        free(db->txn_start_active_entry_bits);
        db->txn_start_active_entry_bits = NULL;
        db->txn_start_active_bit_bytes = 0;
        db->txn_start_active_entry_bit_bytes = 0;
        db->txn_start_entry_count = 0;
        db->txn_start_active_entry_count = 0;
        return rc;
    }
    db->write_txn_active = 0;
    db->batch_index_deferred = 0;
    db->batch_index_dirty = 0;
    free(db->txn_start_active_bits);
    db->txn_start_active_bits = NULL;
    free(db->txn_start_active_entry_bits);
    db->txn_start_active_entry_bits = NULL;
    db->txn_start_active_bit_bytes = 0;
    db->txn_start_active_entry_bit_bytes = 0;
    db->txn_start_delta_count = 0;
    db->txn_start_delta_cap = 0;
    db->txn_start_entry_count = 0;
    db->txn_start_active_entry_count = 0;
    return EZDB_OK;
}

int ezdb_rollback_write(Ezdb* db)
{
    if (!db) return EZDB_ERR_ARG;
    if (!db->write_txn_active) return EZDB_ERR_ARG;
    int rc = restore_txn_snapshot(db);
    db->write_txn_active = 0;
    db->batch_index_deferred = 0;
    db->batch_index_dirty = 0;
    free(db->txn_start_active_bits);
    db->txn_start_active_bits = NULL;
    free(db->txn_start_active_entry_bits);
    db->txn_start_active_entry_bits = NULL;
    db->txn_start_active_bit_bytes = 0;
    db->txn_start_active_entry_bit_bytes = 0;
    db->txn_start_delta_count = 0;
    db->txn_start_delta_cap = 0;
    db->txn_start_entry_count = 0;
    db->txn_start_active_entry_count = 0;
    if (db->fp) {
        if (db->format_v13) {
            int wrc = write_header(db);
            if (wrc != EZDB_OK && rc == EZDB_OK) rc = wrc;
        }
        if (fseek(db->fp, (long)db->header.reserved_offset, SEEK_SET) != 0) return EZDB_ERR_IO;
    }
    return rc;
}

int ezdb_insert_many(Ezdb* db, const EzdbFileRecord* records, uint32_t count, uint32_t* first_id)
{
    if (!db || (!records && count)) return EZDB_ERR_ARG;
    if (db->read_only) return EZDB_ERR_READ_ONLY;
    if ((uint64_t)count > UINT32_MAX - db->header.file_count) return EZDB_ERR_MEMORY;
    int own_txn = db->write_txn_active ? 0 : 1;
    int rc = EZDB_OK;
    if (own_txn) {
        rc = ezdb_begin_write(db, 0);
        if (rc != EZDB_OK) return rc;
    }
    rc = resize_active_bits(db, db->header.file_count + count);
    if (rc == EZDB_OK) rc = ensure_capacity((void**)&db->deltas, sizeof(EzdbDeltaRecord), &db->delta_cap, db->delta_count + count);
    if (rc == EZDB_OK) rc = delta_hash_ensure(db, db->delta_count + count);
    if (rc != EZDB_OK) {
        if (own_txn) (void)ezdb_rollback_write(db);
        return rc;
    }
    uint32_t first = 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t id = 0;
        rc = ezdb_insert(db, &records[i], &id);
        if (rc != EZDB_OK) break;
        if (i == 0) first = id;
    }
    if (rc == EZDB_OK && first_id && count) *first_id = first;
    if (own_txn) {
        if (rc == EZDB_OK) {
            rc = ezdb_commit_write(db);
        } else {
            (void)ezdb_rollback_write(db);
        }
    }
    return rc;
}

int ezdb_insert(Ezdb* db, const EzdbFileRecord* record, uint32_t* out_id)
{
    if (!db || !record || !record->path || !out_id) return EZDB_ERR_ARG;
    if (db->read_only) return EZDB_ERR_READ_ONLY;
    if (db->header.file_count >= UINT32_MAX) return EZDB_ERR_MEMORY;
    uint32_t id = (uint32_t)db->header.file_count;
    uint64_t old_file_count = db->header.file_count;
    uint64_t old_active_count = db->header.active_count;
    uint64_t old_delta_offset = db->header.delta_offset;
    uint64_t old_delta_size = db->header.delta_size;
    uint64_t old_reserved_offset = db->header.reserved_offset;

    db->header.file_count += 1u;
    db->header.active_count += 1u;
    if (ensure_active_bits_zero_extended(db, old_file_count, db->header.file_count) != EZDB_OK) {
        db->header.file_count = old_file_count;
        db->header.active_count = old_active_count;
        return EZDB_ERR_MEMORY;
    }
    bitset_set(db->active_bits, id, 0);
    if (db->archive_first_entry_ids) db->archive_first_entry_ids[id] = UINT32_MAX;

    int rc = append_delta_disk(db, EZDB_DELTA_INSERT, id, record, !db->write_txn_active);
    if (rc == EZDB_OK) rc = append_delta_memory(db, EZDB_DELTA_INSERT, id, record->path, (uint32_t)strlen(record->path), record->size, record->modified_time);
    if (rc == EZDB_OK) {
        bitset_set(db->active_bits, id, 1);
        *out_id = id;
        return EZDB_OK;
    }
    db->header.file_count = old_file_count;
    db->header.active_count = old_active_count;
    db->header.delta_offset = old_delta_offset;
    db->header.delta_size = old_delta_size;
    db->header.reserved_offset = old_reserved_offset;
    (void)resize_active_bits(db, old_file_count);
    return rc;
}

int ezdb_update(Ezdb* db, uint32_t id, const EzdbFileRecord* record)
{
    if (!db || !record || !record->path) return EZDB_ERR_ARG;
    if (db->read_only) return EZDB_ERR_READ_ONLY;
    if (id >= db->header.file_count || !bitset_get(db->active_bits, id)) return EZDB_ERR_NOT_FOUND;
    uint64_t old_delta_offset = db->header.delta_offset;
    uint64_t old_delta_size = db->header.delta_size;
    uint64_t old_reserved_offset = db->header.reserved_offset;
    int rc = append_delta_disk(db, EZDB_DELTA_UPDATE, id, record, !db->write_txn_active);
    if (rc == EZDB_OK) rc = append_delta_memory(db, EZDB_DELTA_UPDATE, id, record->path, (uint32_t)strlen(record->path), record->size, record->modified_time);
    if (rc == EZDB_OK) {
        if (id < db->header.base_file_count) bitset_set(db->covered_base_bits, id, 1);
        bitset_set(db->active_bits, id, 1);
        return EZDB_OK;
    }
    db->header.delta_offset = old_delta_offset;
    db->header.delta_size = old_delta_size;
    db->header.reserved_offset = old_reserved_offset;
    return rc;
}

int ezdb_delete(Ezdb* db, uint32_t id)
{
    if (!db) return EZDB_ERR_ARG;
    if (db->read_only) return EZDB_ERR_READ_ONLY;
    if (id >= db->header.file_count || !bitset_get(db->active_bits, id)) return EZDB_ERR_NOT_FOUND;
    uint64_t old_active_count = db->header.active_count;
    uint64_t old_delta_offset = db->header.delta_offset;
    uint64_t old_delta_size = db->header.delta_size;
    uint64_t old_reserved_offset = db->header.reserved_offset;
    db->header.active_count -= 1u;
    int rc = append_delta_disk(db, EZDB_DELTA_DELETE, id, NULL, !db->write_txn_active);
    if (rc == EZDB_OK) rc = append_delta_memory(db, EZDB_DELTA_DELETE, id, NULL, 0, 0, 0);
    if (rc == EZDB_OK) {
        bitset_set(db->active_bits, id, 0);
        if (id < db->header.base_file_count) bitset_set(db->covered_base_bits, id, 1);
        return EZDB_OK;
    }
    db->header.active_count = old_active_count;
    db->header.delta_offset = old_delta_offset;
    db->header.delta_size = old_delta_size;
    db->header.reserved_offset = old_reserved_offset;
    return rc;
}

int ezdb_upsert_archive(Ezdb* db, const EzdbArchiveRecord* record, uint32_t* out_id)
{
    if (!db || !record || !record->file_path || !out_id) return EZDB_ERR_ARG;
    for (uint32_t i = 0; i < db->header.base_file_count; ++i) {
        if (bitset_get(db->active_bits, i) &&
            db->archive_meta[i].drive_letter == (unsigned char)record->drive_letter &&
            db->archive_meta[i].file_ref_number == record->file_ref_number) {
            EzdbFileRecord file_record;
            file_record.path = record->file_path;
            file_record.size = record->file_size;
            file_record.modified_time = record->modified_time;
            int rc = ezdb_update(db, i, &file_record);
            if (rc == EZDB_OK) {
                db->archive_meta[i].usn = record->usn;
                *out_id = i;
            }
            return rc;
        }
    }
    EzdbFileRecord file_record;
    file_record.path = record->file_path;
    file_record.size = record->file_size;
    file_record.modified_time = record->modified_time;
    return ezdb_insert(db, &file_record, out_id);
}

int ezdb_upsert_archives(Ezdb* db, const EzdbArchiveRecord* records, uint32_t count, uint32_t* out_ids)
{
    if (!db || (!records && count) || (!out_ids && count)) return EZDB_ERR_ARG;
    int own_txn = db->write_txn_active ? 0 : 1;
    int rc = EZDB_OK;
    if (own_txn) {
        rc = ezdb_begin_write(db, 0);
        if (rc != EZDB_OK) return rc;
    }
    for (uint32_t i = 0; i < count; ++i) {
        rc = ezdb_upsert_archive(db, &records[i], &out_ids[i]);
        if (rc != EZDB_OK) break;
    }
    if (own_txn) {
        if (rc == EZDB_OK) rc = ezdb_commit_write(db);
        else (void)ezdb_rollback_write(db);
    }
    return rc;
}

int ezdb_delete_archive_by_ref(Ezdb* db, char drive_letter, uint64_t file_ref_number)
{
    if (!db) return EZDB_ERR_ARG;
    for (uint32_t i = 0; i < db->header.base_file_count; ++i) {
        if (bitset_get(db->active_bits, i) &&
            db->archive_meta[i].drive_letter == (unsigned char)drive_letter &&
            db->archive_meta[i].file_ref_number == file_ref_number) {
            int own_txn = db->write_txn_active ? 0 : 1;
            int rc = EZDB_OK;
            if (own_txn) {
                rc = ezdb_begin_write(db, 0);
                if (rc != EZDB_OK) return rc;
            }
            rc = append_entry_delete_archive_frame(db, i, 0);
            if (rc == EZDB_OK) rc = ezdb_delete(db, i);
            if (own_txn) {
                if (rc == EZDB_OK) rc = ezdb_commit_write(db);
                else (void)ezdb_rollback_write(db);
            }
            return rc;
        }
    }
    return EZDB_ERR_NOT_FOUND;
}

int ezdb_replace_archive_entries(Ezdb* db, uint32_t archive_id, const EzdbEntryRecord* entries, uint32_t entry_count)
{
    if (!db || (!entries && entry_count)) return EZDB_ERR_ARG;
    if (archive_id >= db->header.file_count || !bitset_get(db->active_bits, archive_id)) return EZDB_ERR_NOT_FOUND;
    int rc = ezdb_begin_replace_archive_entries(db, archive_id);
    if (rc == EZDB_OK) rc = ezdb_append_archive_entries(db, archive_id, entries, entry_count);
    if (rc == EZDB_OK) rc = ezdb_finish_replace_archive_entries(db, archive_id);
    else (void)ezdb_abort_replace_archive_entries(db, archive_id);
    return rc;
}

int ezdb_begin_replace_archive_entries(Ezdb* db, uint32_t archive_id)
{
    if (!db) return EZDB_ERR_ARG;
    if (db->read_only) return EZDB_ERR_READ_ONLY;
    return append_entry_delete_archive_frame(db, archive_id, !db->write_txn_active);
}

int ezdb_append_archive_entries(Ezdb* db, uint32_t archive_id, const EzdbEntryRecord* entries, uint32_t entry_count)
{
    if (!db || (!entries && entry_count)) return EZDB_ERR_ARG;
    if (db->read_only) return EZDB_ERR_READ_ONLY;
    if (archive_id >= db->header.file_count || !bitset_get(db->active_bits, archive_id)) return EZDB_ERR_NOT_FOUND;
    if (!entry_count) {
        if (!db->write_txn_active) return write_header(db);
        return EZDB_OK;
    }

    /* Preallocate entry arrays for the entire batch */
    uint64_t old_entry_count = db->header.entry_count;
    uint64_t new_entry_count = old_entry_count + entry_count;
    int rc = resize_entry_arrays(db, new_entry_count);
    if (rc != EZDB_OK) return rc;

    /* Ensure memory is zeroed and initialized for the new entries */
    if (new_entry_count > old_entry_count) {
        size_t add = (size_t)(new_entry_count - old_entry_count);
        memset(db->entry_archive_ids + old_entry_count, 0, sizeof(uint32_t) * add);
        memset(db->entry_path_offsets + old_entry_count, 0, sizeof(uint32_t) * add);
        memset(db->entry_path_lens + old_entry_count, 0, sizeof(uint32_t) * add);
        for (uint64_t i = old_entry_count; i < new_entry_count; ++i) db->entry_next_in_archive[i] = UINT32_MAX;
        memset(db->delta_entry_refs + old_entry_count, 0, sizeof(EzdbDeltaEntryRef) * add);
        /* Zero new bit regions */
        {
            size_t old_bb = ((size_t)old_entry_count + 7u) / 8u;
            size_t new_bb = ((size_t)new_entry_count + 7u) / 8u;
            if (new_bb > old_bb) {
                memset(db->active_entry_bits + old_bb, 0, new_bb - old_bb);
                memset(db->delta_entry_bits + old_bb, 0, new_bb - old_bb);
            }
        }
        if (old_entry_count && (old_entry_count & 7u)) {
            db->active_entry_bits[old_entry_count >> 3u] &= (unsigned char)((1u << (old_entry_count & 7u)) - 1u);
            db->delta_entry_bits[old_entry_count >> 3u] &= (unsigned char)((1u << (old_entry_count & 7u)) - 1u);
        }
    }

    /* Build in-memory delta buffer for the entire batch */
    size_t buf_cap = 0;
    for (uint32_t i = 0; i < entry_count; ++i) {
        size_t path_len = entries[i].entry_path ? strlen(entries[i].entry_path) : 0;
        size_t raw_len = entries[i].entry_raw_path ? entries[i].entry_raw_path_len : 0;
        buf_cap += sizeof(EzdbEntryDeltaDiskHeader) + path_len + raw_len;
    }
    unsigned char* buf = (unsigned char*)malloc(buf_cap ? buf_cap : 1u);
    if (!buf && buf_cap) return EZDB_ERR_MEMORY;
    size_t buf_pos = 0;

    /* Update header counts and build buffer */
    db->header.entry_count = new_entry_count;
    db->header.active_entry_count += entry_count;

    uint64_t append_base = ezdb_delta_append_offset(db);
    for (uint32_t i = 0; i < entry_count; ++i) {
        uint32_t id = (uint32_t)(old_entry_count + i);
        size_t path_len_sz = entries[i].entry_path ? strlen(entries[i].entry_path) : 0;
        uint32_t path_len = (uint32_t)path_len_sz;
        uint32_t raw_len = entries[i].entry_raw_path ? entries[i].entry_raw_path_len : 0;

        EzdbEntryDeltaDiskHeader eh;
        memset(&eh, 0, sizeof(eh));
        eh.magic = EZDB_DELTA_MAGIC;
        eh.type = EZDB_DELTA_ENTRY_APPEND;
        eh.id = id;
        eh.archive_id = archive_id;
        eh.entry_path_len = path_len;
        eh.entry_raw_path_len = raw_len;
        eh.compressed_size = entries[i].compressed_size;
        eh.original_size = entries[i].original_size;
        eh.modified_time = entries[i].modified_time;

        memcpy(buf + buf_pos, &eh, sizeof(eh));
        buf_pos += sizeof(eh);
        if (path_len) { memcpy(buf + buf_pos, entries[i].entry_path, path_len); buf_pos += path_len; }
        if (raw_len) { memcpy(buf + buf_pos, entries[i].entry_raw_path, raw_len); buf_pos += raw_len; }

        /* Update in-memory state */
        uint64_t entry_disk_offset = append_base + (uint64_t)(buf_pos - sizeof(eh) - path_len - raw_len);
        db->entry_archive_ids[id] = archive_id;
        db->entry_path_offsets[id] = id;
        db->entry_path_lens[id] = path_len;
        db->delta_entry_refs[id].path_offset = entry_disk_offset + sizeof(eh);
        db->delta_entry_refs[id].path_len = path_len;
        db->delta_entry_refs[id].raw_offset = entry_disk_offset + sizeof(eh) + path_len;
        db->delta_entry_refs[id].raw_len = raw_len;
        db->delta_entry_refs[id].compressed_size = entries[i].compressed_size;
        db->delta_entry_refs[id].original_size = entries[i].original_size;
        db->delta_entry_refs[id].modified_time = entries[i].modified_time;
        bitset_set(db->active_entry_bits, id, 1);
        bitset_set(db->delta_entry_bits, id, 1);
        link_entry_to_archive(db, id, archive_id);

        /* Deferred gram index: skip during bulk import */
        if (path_len && entries[i].entry_path) {
            rc = delta_entry_index_add_path(db, id, entries[i].entry_path);
            if (rc != EZDB_OK) break;
        }
    }

    /* Single fwrite for the entire batch */
    if (rc == EZDB_OK && buf_cap > 0) {
        if (fseek(db->fp, (long)append_base, SEEK_SET) != 0) rc = EZDB_ERR_IO;
        if (rc == EZDB_OK && fwrite(buf, 1, buf_pos, db->fp) != buf_pos) rc = EZDB_ERR_IO;
        if (rc == EZDB_OK) {
            if (!db->header.delta_offset) db->header.delta_offset = append_base;
            db->header.delta_size += buf_pos;
            db->header.reserved_offset = db->header.delta_offset + db->header.delta_size;
            db->header.reserved_size = 0;
        }
    }
    free(buf);


    if (rc != EZDB_OK) {
        /* Rollback in-memory state on error */
        db->header.entry_count = old_entry_count;
        db->header.active_entry_count -= entry_count;
        return rc;
    }
    if (!db->write_txn_active) rc = write_header(db);
    return rc;
}

int ezdb_finish_replace_archive_entries(Ezdb* db, uint32_t archive_id)
{
    if (!db) return EZDB_ERR_ARG;
    if (archive_id >= db->header.file_count || !bitset_get(db->active_bits, archive_id)) return EZDB_ERR_NOT_FOUND;
    return db->write_txn_active ? EZDB_OK : write_header(db);
}

int ezdb_abort_replace_archive_entries(Ezdb* db, uint32_t archive_id)
{
    if (!db) return EZDB_ERR_ARG;
    if (archive_id >= db->header.file_count || !bitset_get(db->active_bits, archive_id)) return EZDB_ERR_NOT_FOUND;
    return append_entry_delete_archive_frame(db, archive_id, !db->write_txn_active);
}

static char* ezdb_meta_path(Ezdb* db)
{
    if (!db || !db->path) return NULL;
    size_t len = strlen(db->path);
    const char* suffix = ".meta";
    char* out = (char*)malloc(len + strlen(suffix) + 1u);
    if (!out) return NULL;
    memcpy(out, db->path, len);
    strcpy(out + len, suffix);
    return out;
}

int ezdb_get_meta(Ezdb* db, const char* key, char** out_value)
{
    if (!db || !key || !out_value) return EZDB_ERR_ARG;
    *out_value = NULL;
    char* path = ezdb_meta_path(db);
    if (!path) return EZDB_ERR_MEMORY;
    FILE* fp = fopen(path, "rb");
    free(path);
    if (!fp) return EZDB_ERR_NOT_FOUND;
    char line[4096];
    int rc = EZDB_ERR_NOT_FOUND;
    size_t key_len = strlen(key);
    while (fgets(line, sizeof(line), fp)) {
        char* tab = strchr(line, '\t');
        if (!tab) continue;
        size_t len = (size_t)(tab - line);
        if (len == key_len && memcmp(line, key, key_len) == 0) {
            char* value = tab + 1;
            size_t value_len = strlen(value);
            while (value_len && (value[value_len - 1u] == '\n' || value[value_len - 1u] == '\r')) --value_len;
            *out_value = ezdb_strdup_range(value, value_len);
            rc = *out_value ? EZDB_OK : EZDB_ERR_MEMORY;
            break;
        }
    }
    fclose(fp);
    return rc;
}

int ezdb_put_meta(Ezdb* db, const char* key, const char* value)
{
    if (!db || !key || !value || strchr(key, '\t') || strchr(key, '\n') || strchr(key, '\r')) return EZDB_ERR_ARG;
    char* path = ezdb_meta_path(db);
    if (!path) return EZDB_ERR_MEMORY;
    FILE* in = fopen(path, "rb");
    char* tmp_path = (char*)malloc(strlen(path) + 5u);
    if (!tmp_path) {
        if (in) fclose(in);
        free(path);
        return EZDB_ERR_MEMORY;
    }
    sprintf(tmp_path, "%s.tmp", path);
    FILE* out = fopen(tmp_path, "wb");
    if (!out) {
        if (in) fclose(in);
        free(tmp_path);
        free(path);
        return EZDB_ERR_IO;
    }
    char line[4096];
    int wrote = 0;
    size_t key_len = strlen(key);
    if (in) {
        while (fgets(line, sizeof(line), in)) {
            char* tab = strchr(line, '\t');
            int same = 0;
            if (tab) {
                size_t len = (size_t)(tab - line);
                same = len == key_len && memcmp(line, key, key_len) == 0;
            }
            if (same) {
                fprintf(out, "%s\t%s\n", key, value);
                wrote = 1;
            } else {
                fputs(line, out);
            }
        }
        fclose(in);
    }
    if (!wrote) fprintf(out, "%s\t%s\n", key, value);
    int rc = fclose(out) == 0 ? EZDB_OK : EZDB_ERR_IO;
    if (rc == EZDB_OK) {
        remove(path);
        if (rename(tmp_path, path) != 0) rc = EZDB_ERR_IO;
    }
    if (rc != EZDB_OK) remove(tmp_path);
    free(tmp_path);
    free(path);
    return rc;
}

int ezdb_compact(Ezdb* db)
{
    if (!db) return EZDB_ERR_ARG;
    if (db->read_only) return EZDB_ERR_READ_ONLY;
    if (!db->path) return EZDB_ERR_ARG;
    uint32_t active_archives = ezdb_active_count(db);
    uint32_t active_entries = ezdb_compute_active_entry_count(db);
    EzdbArchiveRecord* archives = (EzdbArchiveRecord*)calloc(active_archives ? active_archives : 1u, sizeof(EzdbArchiveRecord));
    char** archive_paths = (char**)calloc(active_archives ? active_archives : 1u, sizeof(char*));
    uint32_t* archive_id_map = (uint32_t*)malloc(sizeof(uint32_t) * (size_t)(db->header.file_count ? db->header.file_count : 1u));
    uint32_t* build_archive_id_map = NULL;
    if (!archives || !archive_paths || !archive_id_map) {
        free(archives);
        free(archive_paths);
        free(archive_id_map);
        return EZDB_ERR_MEMORY;
    }
    for (uint32_t i = 0; i < db->header.file_count; ++i) archive_id_map[i] = UINT32_MAX;
    uint32_t out_archive_count = 0;
    int rc = EZDB_OK;
    for (uint32_t i = 0; rc == EZDB_OK && i < db->header.file_count; ++i) {
        EzdbSearchResult result;
        rc = build_result_path(db, i, &result);
        if (rc == EZDB_ERR_NOT_FOUND) {
            rc = EZDB_OK;
            continue;
        }
        if (rc != EZDB_OK) break;
        archive_id_map[i] = out_archive_count;
        archive_paths[out_archive_count] = result.path;
        archives[out_archive_count].file_path = archive_paths[out_archive_count];
        archives[out_archive_count].file_size = result.size;
        archives[out_archive_count].modified_time = result.modified_time;
        if (i < db->header.base_file_count && db->archive_meta) {
            archives[out_archive_count].drive_letter = (char)db->archive_meta[i].drive_letter;
            archives[out_archive_count].file_ref_number = db->archive_meta[i].file_ref_number;
            archives[out_archive_count].usn = db->archive_meta[i].usn;
        }
        ++out_archive_count;
    }
    if (rc != EZDB_OK) {
        for (uint32_t i = 0; i < out_archive_count; ++i) free(archive_paths[i]);
        free(archives);
        free(archive_paths);
        free(archive_id_map);
        return rc;
    }
    build_archive_id_map = (uint32_t*)malloc(sizeof(uint32_t) * (size_t)(out_archive_count ? out_archive_count : 1u));
    if (!build_archive_id_map) {
        for (uint32_t i = 0; i < out_archive_count; ++i) free(archive_paths[i]);
        free(archives);
        free(archive_paths);
        free(archive_id_map);
        return EZDB_ERR_MEMORY;
    }
    for (uint32_t i = 0; i < out_archive_count; ++i) build_archive_id_map[i] = UINT32_MAX;

    EzdbCompactEntrySource compact_source;
    memset(&compact_source, 0, sizeof(compact_source));
    compact_source.archive_id_map = archive_id_map;
    compact_source.active_entry_bits = db->active_entry_bits;
    compact_source.active_archive_bits = db->active_bits;
    compact_source.file_count = (uint32_t)db->header.file_count;
    compact_source.detail_store = entry_detail_store(db);
    compact_source.path_store = entry_path_store(db);
    EzdbEntrySource source;
    memset(&source, 0, sizeof(source));
    source.user_data = &compact_source;
    source.reset = ezdb_entries_compact_source_reset;
    source.next = ezdb_entries_compact_source_next;

    size_t path_len = strlen(db->path);
    char* tmp_path = (char*)malloc(path_len + 13u);
    if (!tmp_path) rc = EZDB_ERR_MEMORY;
    if (rc == EZDB_OK) {
        sprintf(tmp_path, "%s.compact.tmp", db->path);
        remove(tmp_path);
        EzdbBuildOptionsResolved options;
        rc = resolve_build_options(tmp_path, NULL, &options);
        if (rc == EZDB_OK) {
            rc = ezdb_write_archive_base(archives, out_archive_count, NULL, active_entries, tmp_path, build_archive_id_map, active_entries ? &source : NULL, &options);
        }
    }
    ezdb_entries_compact_source_clear_current(&compact_source);
    if (rc == EZDB_OK) {
        if (db->fp) {
            fclose(db->fp);
            db->fp = NULL;
        }
        if (remove(db->path) != 0 || rename(tmp_path, db->path) != 0) {
            rc = EZDB_ERR_IO;
        }
    }
    if (rc == EZDB_OK) {
        char* reopen_path = ezdb_strdup_range(db->path, strlen(db->path));
        Ezdb* reopened = NULL;
        if (!reopen_path) rc = EZDB_ERR_MEMORY;
        else {
            rc = ezdb_open(reopen_path, &reopened);
        }
        if (rc == EZDB_OK) {
            char* old_path = db->path;
            ezdb_release_members(db, 0);
            *db = *reopened;
            db->path = old_path;
            free(reopened->path);
            free(reopened);
        }
        free(reopen_path);
        if (rc != EZDB_OK) {
            db->fp = fopen(db->path, "r+b");
        }
    }
    if (rc != EZDB_OK && tmp_path) remove(tmp_path);
    free(tmp_path);
    for (uint32_t i = 0; i < out_archive_count; ++i) free(archive_paths[i]);
    free(archives);
    free(archive_paths);
    free(archive_id_map);
    free(build_archive_id_map);
    return rc;
}

const char* ezdb_error_message(int code)
{
    switch (code) {
    case EZDB_OK: return "ok";
    case EZDB_ERR_ARG: return "invalid argument";
    case EZDB_ERR_IO: return "I/O error";
    case EZDB_ERR_FORMAT: return "invalid ezdb format";
    case EZDB_ERR_MEMORY: return "out of memory";
    case EZDB_ERR_NOT_FOUND: return "not found";
    case EZDB_ERR_READ_ONLY: return "database is read-only";
    default: return "unknown error";
    }
}
