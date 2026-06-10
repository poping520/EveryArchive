#include "ezdb_delta.h"

#include "ezdb_build.h"
#include "ezdb_core_internal.h"
#include "ezdb_entries.h"
#include "ezdb_format.h"
#include "ezdb_postings.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- Bulk write mode helpers --- */

/* --- Memory-backed entry stream with range support (for parallel index build) --- */

typedef struct MemoryStreamCtx {
    const EzdbEntryRecord* entries;
    uint32_t entry_count;
    uint32_t pos;
    uint32_t range_end;
    const uint32_t* archive_bases;
    const uint32_t* archive_counts;
    uint32_t archive_count;
} MemoryStreamCtx;

typedef struct MemoryRangeCtx {
    const EzdbEntryRecord* entries;
    uint32_t start;
    uint32_t end;
    uint32_t pos;
} MemoryRangeCtx;

static int mem_stream_next(void* user_data, EzdbEntryRecord* out)
{
    MemoryStreamCtx* c = (MemoryStreamCtx*)user_data;
    if (c->pos >= c->range_end) return EZDB_ERR_NOT_FOUND;
    *out = c->entries[c->pos++];
    return EZDB_OK;
}

static int mem_stream_reset(void* user_data)
{
    MemoryStreamCtx* c = (MemoryStreamCtx*)user_data;
    c->pos = 0;
    c->range_end = c->entry_count;
    return EZDB_OK;
}

static int mem_stream_reset_range(void* user_data, uint32_t archive_begin, uint32_t archive_end)
{
    MemoryStreamCtx* c = (MemoryStreamCtx*)user_data;
    if (archive_begin >= c->archive_count || archive_end <= archive_begin) {
        c->pos = c->entry_count;
        c->range_end = c->entry_count;
        return EZDB_OK;
    }
    uint32_t real_end = (archive_end <= c->archive_count) ? archive_end : c->archive_count;
    c->pos = c->archive_bases[archive_begin];
    c->range_end = c->archive_bases[real_end - 1u] + c->archive_counts[real_end - 1u];
    return EZDB_OK;
}

static int range_stream_next(void* user_data, EzdbEntryRecord* out)
{
    MemoryRangeCtx* c = (MemoryRangeCtx*)user_data;
    if (c->pos >= c->end) return EZDB_ERR_NOT_FOUND;
    *out = c->entries[c->pos++];
    return EZDB_OK;
}

static int range_stream_reset(void* user_data)
{
    MemoryRangeCtx* c = (MemoryRangeCtx*)user_data;
    c->pos = c->start;
    return EZDB_OK;
}

static int mem_stream_open_range(void* user_data, uint32_t archive_begin, uint32_t archive_end, EzdbEntryStream* out)
{
    MemoryStreamCtx* c = (MemoryStreamCtx*)user_data;
    if (!out) return EZDB_ERR_ARG;
    MemoryRangeCtx* rc = (MemoryRangeCtx*)calloc(1, sizeof(*rc));
    if (!rc) return EZDB_ERR_MEMORY;
    rc->entries = c->entries;
    if (archive_begin >= c->archive_count || archive_end <= archive_begin) {
        rc->start = 0;
        rc->end = 0;
        rc->pos = 0;
    } else {
        uint32_t real_end = (archive_end <= c->archive_count) ? archive_end : c->archive_count;
        rc->start = c->archive_bases[archive_begin];
        rc->end = c->archive_bases[real_end - 1u] + c->archive_counts[real_end - 1u];
        rc->pos = rc->start;
    }
    memset(out, 0, sizeof(*out));
    out->user_data = rc;
    out->reset = range_stream_reset;
    out->next = range_stream_next;
    return EZDB_OK;
}

static void mem_stream_close_range(EzdbEntryStream* stream)
{
    if (stream && stream->user_data) {
        free(stream->user_data);
        stream->user_data = NULL;
    }
}

static int cmp_entry_by_archive_id(const void* a, const void* b)
{
    uint32_t id_a = ((const EzdbEntryRecord*)a)->archive_id;
    uint32_t id_b = ((const EzdbEntryRecord*)b)->archive_id;
    return (id_a > id_b) - (id_a < id_b);
}

static void bulk_free_entries(EzdbEntryRecord* be, char** bep, void** berp, uint32_t ec)
{
    for (uint32_t i = 0; i < ec; ++i) {
        if (bep && bep[i]) free(bep[i]);
        if (berp && berp[i]) free(berp[i]);
    }
    free(be);
    free(bep);
    free(berp);
}

static void ezdb_bulk_free(Ezdb* db)
{
    if (!db) return;
    for (uint32_t i = 0; i < db->bulk_archive_count; ++i) {
        if (db->bulk_archive_paths && db->bulk_archive_paths[i]) free(db->bulk_archive_paths[i]);
    }
    free(db->bulk_archives);
    free(db->bulk_archive_paths);
    free(db->bulk_archive_id_map);
    for (uint32_t i = 0; i < db->bulk_entry_count; ++i) {
        if (db->bulk_entry_paths && db->bulk_entry_paths[i]) free(db->bulk_entry_paths[i]);
        if (db->bulk_entry_raw_paths && db->bulk_entry_raw_paths[i]) free(db->bulk_entry_raw_paths[i]);
    }
    free(db->bulk_entries);
    free(db->bulk_entry_paths);
    free(db->bulk_entry_raw_paths);
    db->bulk_archives = NULL;
    db->bulk_archive_paths = NULL;
    db->bulk_archive_id_map = NULL;
    db->bulk_entries = NULL;
    db->bulk_entry_paths = NULL;
    db->bulk_entry_raw_paths = NULL;
    db->bulk_archive_count = 0;
    db->bulk_archive_cap = 0;
    db->bulk_entry_count = 0;
    db->bulk_entry_cap = 0;
}

static void ezdb_path_cache_free(Ezdb* db)
{
    if (!db || !db->delta_entry_path_cache) return;
    for (uint32_t i = 0; i < db->delta_entry_path_cache_cap; ++i) {
        if (db->delta_entry_path_cache[i]) free(db->delta_entry_path_cache[i]);
    }
    free(db->delta_entry_path_cache);
    db->delta_entry_path_cache = NULL;
    db->delta_entry_path_cache_cap = 0;
}

static int ezdb_path_cache_ensure(Ezdb* db, uint32_t needed)
{
    if (needed <= db->delta_entry_path_cache_cap) return EZDB_OK;
    uint32_t new_cap = db->delta_entry_path_cache_cap ? db->delta_entry_path_cache_cap : 64u;
    while (new_cap < needed) new_cap *= 2u;
    char** new_arr = (char**)realloc(db->delta_entry_path_cache, sizeof(char*) * (size_t)new_cap);
    if (!new_arr) return EZDB_ERR_MEMORY;
    memset(new_arr + db->delta_entry_path_cache_cap, 0, sizeof(char*) * (size_t)(new_cap - db->delta_entry_path_cache_cap));
    db->delta_entry_path_cache = new_arr;
    db->delta_entry_path_cache_cap = new_cap;
    return EZDB_OK;
}

/* --- Delta hash table --- */

static uint32_t delta_bucket_for(uint32_t id, uint32_t bucket_count)
{
    uint32_t x = id;
    x ^= x >> 16u;
    x *= 0x7feb352du;
    x ^= x >> 15u;
    return x & (bucket_count - 1u);
}

void ezdb_delta_hash_reset(Ezdb* db)
{
    if (!db->delta_buckets || !db->delta_bucket_count) return;
    for (uint32_t i = 0; i < db->delta_bucket_count; ++i) db->delta_buckets[i] = UINT32_MAX;
    for (uint32_t i = 0; i < db->delta_count; ++i) {
        uint32_t bucket = delta_bucket_for(db->deltas[i].id, db->delta_bucket_count);
        db->deltas[i].next_by_id = db->delta_buckets[bucket];
        db->delta_buckets[bucket] = i;
    }
}

int ezdb_delta_hash_ensure(Ezdb* db, uint32_t needed_records)
{
    uint32_t wanted = ezdb_next_pow2_u32(needed_records * 2u + 16u);
    if (wanted < 16u) wanted = 16u;
    if (db->delta_bucket_count >= wanted) return EZDB_OK;
    uint32_t* buckets = (uint32_t*)malloc(sizeof(uint32_t) * (size_t)wanted);
    if (!buckets) return EZDB_ERR_MEMORY;
    free(db->delta_buckets);
    db->delta_buckets = buckets;
    db->delta_bucket_count = wanted;
    ezdb_delta_hash_reset(db);
    return EZDB_OK;
}

int ezdb_delta_hash_add_latest(Ezdb* db, uint32_t delta_index)
{
    if (!db || delta_index >= db->delta_count) return EZDB_ERR_ARG;
    if (ezdb_delta_hash_ensure(db, db->delta_count) != EZDB_OK) return EZDB_ERR_MEMORY;
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

EzdbDeltaRecord* ezdb_find_delta_record(Ezdb* db, uint32_t id)
{
    if (!db || !db->delta_buckets || !db->delta_bucket_count) return NULL;
    uint32_t bucket = delta_bucket_for(id, db->delta_bucket_count);
    for (uint32_t i = db->delta_buckets[bucket]; i != UINT32_MAX; i = db->deltas[i].next_by_id) {
        if (db->deltas[i].id == id) return &db->deltas[i];
    }
    return NULL;
}
int ezdb_append_delta_memory(Ezdb* db, uint32_t type, uint32_t id, const char* path, uint32_t path_len, uint64_t size, uint64_t modified_time)
{
    if (ezdb_delta_hash_ensure(db, db->delta_count + 1u) != EZDB_OK) return EZDB_ERR_MEMORY;

    EzdbDeltaRecord* existing = db->write_txn_active ? NULL : ezdb_find_delta_record(db, id);
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

    if (ezdb_ensure_capacity((void**)&db->deltas, sizeof(EzdbDeltaRecord), &db->delta_cap, db->delta_count + 1u) != EZDB_OK) {
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
    int rc = ezdb_delta_hash_add_latest(db, db->delta_count - 1u);
    if (rc != EZDB_OK) {
        free(rec->path);
        db->delta_count -= 1u;
        ezdb_delta_hash_reset(db);
    }
    return rc;
}
int ezdb_delta_entry_index_add_path(Ezdb* db, uint32_t entry_id, const char* path)
{
    if (!path || !*path) return EZDB_OK;
    if (db->batch_index_deferred) { db->batch_index_dirty = 1; return EZDB_OK; }
    int rc = ezdb_delta_entry_index_ensure(db);
    if (rc != EZDB_OK) return rc;
    return ezdb_postings_add_text_grams(&db->delta_entry_index, path, entry_id, 0);
}

int ezdb_delta_entry_index_remove_path(Ezdb* db, uint32_t entry_id, const char* path)
{
    if (!db || !path || !*path || !db->delta_entry_index_ready) return EZDB_OK;
    if (db->batch_index_deferred) { db->batch_index_dirty = 1; return EZDB_OK; }
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

int ezdb_rebuild_delta_entry_index(Ezdb* db)
{
    if (!db) return EZDB_ERR_ARG;
    ezdb_postings_builder_free(&db->delta_entry_index);
    memset(&db->delta_entry_index, 0, sizeof(db->delta_entry_index));
    db->delta_entry_index_ready = 0;

    uint32_t active_delta_count = 0;
    for (uint32_t e = 0; e < db->header.entry_count; ++e) {
        if (ezdb_bitset_get(db->active_entry_bits, e) &&
            ezdb_bitset_get(db->delta_entry_bits, e)) {
            ++active_delta_count;
        }
    }
    if (!active_delta_count) return EZDB_OK;

    uint32_t estimated_keys = active_delta_count * 20u;
    if (estimated_keys < 4096u) estimated_keys = 4096u;
    uint32_t bucket_count = ezdb_next_pow2_u32(estimated_keys * 2u);
    int rc = ezdb_postings_builder_init(&db->delta_entry_index, bucket_count);
    if (rc != EZDB_OK) return rc;
    db->delta_entry_index_ready = 1;

    EzdbEntryPathStore path_store = ezdb_entry_path_store(db);
    int have_cache = db->delta_entry_path_cache != NULL;

    /* Count pass: use path cache if available, otherwise read from disk */
    for (uint32_t e = 0; e < db->header.entry_count; ++e) {
        if (!ezdb_bitset_get(db->active_entry_bits, e) ||
            !ezdb_bitset_get(db->delta_entry_bits, e)) continue;
        const char* cached = (have_cache && e < db->delta_entry_path_cache_cap)
            ? db->delta_entry_path_cache[e] : NULL;
        if (cached) {
            rc = ezdb_postings_count_text_grams(&db->delta_entry_index, cached, e);
        } else {
            char* entry_path = ezdb_entries_copy_path(&path_store, e);
            if (!entry_path) { rc = EZDB_ERR_MEMORY; break; }
            rc = ezdb_postings_count_text_grams(&db->delta_entry_index, entry_path, e);
            free(entry_path);
        }
        if (rc != EZDB_OK) break;
    }
    if (rc != EZDB_OK) return rc;

    rc = ezdb_postings_builder_prepare_fill(&db->delta_entry_index);
    if (rc != EZDB_OK) return rc;

    /* Fill pass */
    for (uint32_t e = 0; e < db->header.entry_count; ++e) {
        if (!ezdb_bitset_get(db->active_entry_bits, e) ||
            !ezdb_bitset_get(db->delta_entry_bits, e)) continue;
        const char* cached = (have_cache && e < db->delta_entry_path_cache_cap)
            ? db->delta_entry_path_cache[e] : NULL;
        if (cached) {
            rc = ezdb_postings_fill_text_grams(&db->delta_entry_index, cached, e);
        } else {
            char* entry_path = ezdb_entries_copy_path(&path_store, e);
            if (!entry_path) { rc = EZDB_ERR_MEMORY; break; }
            rc = ezdb_postings_fill_text_grams(&db->delta_entry_index, entry_path, e);
            free(entry_path);
        }
        if (rc != EZDB_OK) break;
    }
    return rc;
}
int ezdb_replay_delta_log(Ezdb* db)
{
    if (!db->header.delta_offset || !db->header.delta_size) return EZDB_OK;
    db->batch_index_deferred = 1;
    if (_fseeki64(db->fp, (__int64)db->header.delta_offset, SEEK_SET) != 0) return EZDB_ERR_IO;
    uint64_t remaining = db->header.delta_size;
    while (remaining) {
        if (remaining < sizeof(EzdbDeltaDiskHeader)) return EZDB_ERR_FORMAT;
        uint64_t frame_offset = (uint64_t)_ftelli64(db->fp);
        uint32_t magic = 0, type = 0;
        if (fread(&magic, sizeof(magic), 1, db->fp) != 1 ||
            fread(&type, sizeof(type), 1, db->fp) != 1) return EZDB_ERR_IO;
        if (_fseeki64(db->fp, (__int64)frame_offset, SEEK_SET) != 0) return EZDB_ERR_IO;
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
            uint64_t path_offset = (uint64_t)_ftelli64(db->fp);
            uint64_t raw_offset = path_offset + eh.entry_path_len;
            if (_fseeki64(db->fp, (__int64)(eh.entry_path_len + eh.entry_raw_path_len), SEEK_CUR) != 0) return EZDB_ERR_IO;
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
            if (!ezdb_bitset_get(db->active_entry_bits, eh.id)) db->header.active_entry_count += 1u;
            ezdb_bitset_set(db->active_entry_bits, eh.id, 1);
            ezdb_bitset_set(db->delta_entry_bits, eh.id, 1);
            ezdb_link_entry_to_archive(db, eh.id, eh.archive_id);
            EzdbEntryPathStore path_store = ezdb_entry_path_store(db);
            char* entry_path = ezdb_entries_copy_path(&path_store, eh.id);
            if (entry_path) {
                int rc = ezdb_delta_entry_index_add_path(db, eh.id, entry_path);
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
            int rc = ezdb_deactivate_entries_for_archive(db, dh.id);
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

        int rc = ezdb_append_delta_memory(db, dh.type, dh.id, path ? path : "", dh.path_len, dh.size, dh.modified_time);
        free(path);
        if (rc != EZDB_OK) return rc;

        if (dh.id < db->header.base_file_count) ezdb_bitset_set(db->covered_base_bits, dh.id, 1);
        if (dh.type == EZDB_DELTA_DELETE) {
            ezdb_bitset_set(db->active_bits, dh.id, 0);
        } else {
            ezdb_bitset_set(db->active_bits, dh.id, 1);
        }
    }
    if (db->batch_index_dirty) {
        int rc2 = ezdb_rebuild_delta_entry_index(db);
        if (rc2 != EZDB_OK) return rc2;
    }
    db->batch_index_deferred = 0;
    db->batch_index_dirty = 0;
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
    ezdb_delta_hash_reset(db);
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
    int rc = ezdb_resize_entry_arrays(db, db->txn_start_entry_count);
    if (rc != EZDB_OK) return rc;
    db->header.entry_count = db->txn_start_entry_count;
    db->header.active_entry_count = db->txn_start_active_entry_count;
    memcpy(db->active_entry_bits, db->txn_start_active_entry_bits, db->txn_start_active_entry_bit_bytes ? db->txn_start_active_entry_bit_bytes : 1u);
    size_t entry_bit_bytes = ((size_t)db->header.entry_count + 7u) / 8u;
    if (db->active_entry_bits_cap_bytes > entry_bit_bytes) {
        memset(db->active_entry_bits + entry_bit_bytes, 0, db->active_entry_bits_cap_bytes - entry_bit_bytes);
        memset(db->delta_entry_bits + entry_bit_bytes, 0, db->delta_entry_bits_cap_bytes - entry_bit_bytes);
    }
    ezdb_rebuild_archive_entry_links(db);
    return ezdb_rebuild_delta_entry_index(db);
}
static int append_delta_disk(Ezdb* db, uint32_t type, uint32_t id, const char* path, uint64_t size, uint64_t modified_time, int flush_now)
{
    if (!db || db->read_only || !db->fp) return EZDB_ERR_READ_ONLY;
    uint32_t path_len = 0;
    if (path) {
        size_t len = strlen(path);
        if (len > UINT32_MAX) return EZDB_ERR_ARG;
        path_len = (uint32_t)len;
    }
    if ((type == EZDB_DELTA_INSERT || type == EZDB_DELTA_UPDATE) && (!path || !path_len)) {
        return EZDB_ERR_ARG;
    }

    uint64_t append_offset = ezdb_delta_append_offset(db);
    if (_fseeki64(db->fp, (__int64)append_offset, SEEK_SET) != 0) return EZDB_ERR_IO;
    EzdbDeltaDiskHeader dh;
    memset(&dh, 0, sizeof(dh));
    dh.magic = EZDB_DELTA_MAGIC;
    dh.type = type;
    dh.id = id;
    dh.path_len = path_len;
    dh.size = size;
    dh.modified_time = modified_time;
    if (fwrite(&dh, sizeof(dh), 1, db->fp) != 1) return EZDB_ERR_IO;
    if (path_len && fwrite(path, 1, path_len, db->fp) != path_len) return EZDB_ERR_IO;

    if (!db->header.delta_offset) db->header.delta_offset = append_offset;
    db->header.delta_size += sizeof(dh) + path_len;
    db->header.reserved_offset = db->header.delta_offset + db->header.delta_size;
    db->header.reserved_size = 0;
    if (!flush_now) return EZDB_OK;
    return ezdb_write_header(db);
}

static int append_delta_frame(Ezdb* db, uint32_t type)
{
    if (!db || db->read_only || !db->fp) return EZDB_ERR_READ_ONLY;
    if (type != EZDB_DELTA_BATCH_BEGIN && type != EZDB_DELTA_BATCH_COMMIT) return EZDB_ERR_ARG;
    uint64_t append_offset = ezdb_delta_append_offset(db);
    if (_fseeki64(db->fp, (__int64)append_offset, SEEK_SET) != 0) return EZDB_ERR_IO;
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
    if (archive_id >= db->header.file_count || !ezdb_bitset_get(db->active_bits, archive_id)) return EZDB_ERR_NOT_FOUND;
    uint64_t append_offset = ezdb_delta_append_offset(db);
    if (_fseeki64(db->fp, (__int64)append_offset, SEEK_SET) != 0) return EZDB_ERR_IO;
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
    int rc;
    if (db->archive_first_entry_ids && archive_id < db->header.file_count &&
        db->archive_first_entry_ids[archive_id] == UINT32_MAX) {
        rc = EZDB_OK;
    } else {
        rc = ezdb_deactivate_entries_for_archive(db, archive_id);
    }
    if (rc != EZDB_OK) return rc;
    if (!flush_now) return EZDB_OK;
    return ezdb_write_header(db);
}

static int append_entry_delta_disk(Ezdb* db, uint32_t archive_id, const EzdbEntryRecord* record, uint32_t* out_id, int flush_now)
{
    if (!db || db->read_only || !db->fp || !record || !record->entry_path || !out_id) return EZDB_ERR_ARG;
    if (archive_id >= db->header.file_count || !ezdb_bitset_get(db->active_bits, archive_id)) return EZDB_ERR_NOT_FOUND;
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
    if (_fseeki64(db->fp, (__int64)append_offset, SEEK_SET) != 0) rc = EZDB_ERR_IO;
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
        ezdb_bitset_set(db->active_entry_bits, id, 1);
        ezdb_bitset_set(db->delta_entry_bits, id, 1);
        ezdb_link_entry_to_archive(db, id, archive_id);
        rc = ezdb_delta_entry_index_add_path(db, id, record->entry_path);
        if (rc == EZDB_OK) {
            *out_id = id;
            return flush_now ? ezdb_write_header(db) : EZDB_OK;
        }
        /* Rollback in-memory entry state on index add failure */
        ezdb_bitset_set(db->active_entry_bits, id, 0);
        ezdb_bitset_set(db->delta_entry_bits, id, 0);
        memset(&db->delta_entry_refs[id], 0, sizeof(db->delta_entry_refs[id]));
        db->entry_archive_ids[id] = 0;
        db->entry_path_offsets[id] = 0;
        db->entry_path_lens[id] = 0;
    }

    db->header.entry_count = old_entry_count;
    db->header.active_entry_count = old_active_entry_count;
    db->header.delta_offset = old_delta_offset;
    db->header.delta_size = old_delta_size;
    db->header.reserved_offset = old_reserved_offset;
    return rc;
}

static void cleanup_txn_state(Ezdb* db)
{
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
    db->write_txn_active = 0;
    db->batch_index_deferred = 0;
    db->batch_index_dirty = 0;
    ezdb_path_cache_free(db);
}

int ezdb_begin_write(Ezdb* db, uint32_t flags)
{
    if (!db) return EZDB_ERR_ARG;
    if (db->read_only) return EZDB_ERR_READ_ONLY;
    if (db->write_txn_active) return EZDB_ERR_ARG;

    if (flags & EZDB_WRITE_BULK) {
        /* Bulk mode: buffer everything in memory, build at commit */
        db->bulk_write_mode = 1;
        db->bulk_archives = NULL;
        db->bulk_archive_paths = NULL;
        db->bulk_archive_count = 0;
        db->bulk_archive_cap = 0;
        db->bulk_archive_id_map = NULL;
        db->bulk_entries = NULL;
        db->bulk_entry_paths = NULL;
        db->bulk_entry_raw_paths = NULL;
        db->bulk_entry_count = 0;
        db->bulk_entry_cap = 0;
        db->write_txn_active = 1;
        db->batch_index_deferred = 1;
        db->batch_index_dirty = 0;
        /* No delta frame needed in bulk mode */
        return EZDB_OK;
    }

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
    int rc;

    /* --- Bulk mode: build database from buffered data --- */
    if (db->bulk_write_mode) {
        /* Detach bulk data from db (we'll close db soon) */
        uint32_t bc = db->bulk_archive_count;
        EzdbArchiveRecord* ba = db->bulk_archives;
        char** bap = db->bulk_archive_paths;
        uint32_t* baim = db->bulk_archive_id_map;
        uint32_t ec = db->bulk_entry_count;
        EzdbEntryRecord* be = db->bulk_entries;
        char** bep = db->bulk_entry_paths;
        void** berp = db->bulk_entry_raw_paths;

        /* Clear bulk fields so ezdb_close doesn't double-free */
        db->bulk_archives = NULL;
        db->bulk_archive_paths = NULL;
        db->bulk_archive_id_map = NULL;
        db->bulk_entries = NULL;
        db->bulk_entry_paths = NULL;
        db->bulk_entry_raw_paths = NULL;
        db->bulk_archive_count = 0;
        db->bulk_entry_count = 0;
        db->bulk_write_mode = 0;
        db->write_txn_active = 0;

        /* Archives may have been upserted before bulk mode started (bc == 0).
           In that case, extract them from the current database. */
        int own_archives = (bc > 0);
        uint32_t own_archive_id_map_cap = 0;
        if (!own_archives && ec > 0) {
            /* Gather archives from current db, same pattern as ezdb_compact */
            bc = 0;
            for (uint32_t i = 0; i < db->header.file_count; ++i) {
                if (ezdb_bitset_get(db->active_bits, i)) ++bc;
            }
            if (bc) {
                ba = (EzdbArchiveRecord*)calloc(bc, sizeof(EzdbArchiveRecord));
                bap = (char**)calloc(bc, sizeof(char*));
                baim = (uint32_t*)malloc(sizeof(uint32_t) * (size_t)(db->header.file_count ? db->header.file_count : 1u));
                if (!ba || !bap || !baim) { rc = EZDB_ERR_MEMORY; goto bulk_cleanup; }
                memset(baim, 0xFF, sizeof(uint32_t) * (size_t)(db->header.file_count ? db->header.file_count : 1u));
                uint32_t out_idx = 0;
                for (uint32_t i = 0; i < db->header.file_count; ++i) {
                    if (!ezdb_bitset_get(db->active_bits, i)) continue;
                    char* path = NULL;
                    rc = ezdb_build_result_path(db, i, &path);
                    if (rc == EZDB_ERR_NOT_FOUND) { rc = EZDB_OK; continue; }
                    if (rc != EZDB_OK) break;
                    bap[out_idx] = path; /* takes ownership */
                    ba[out_idx].file_path = path;
                    ba[out_idx].file_size = ezdb_file_size_by_id(db, i);
                    ba[out_idx].modified_time = ezdb_file_modified_time_by_id(db, i);
                    if (i < db->header.base_file_count && db->archive_meta) {
                        ba[out_idx].drive_letter = (char)db->archive_meta[i].drive_letter;
                        ba[out_idx].file_ref_number = db->archive_meta[i].file_ref_number;
                        ba[out_idx].usn = db->archive_meta[i].usn;
                    }
                    baim[i] = out_idx;
                    ++out_idx;
                }
                if (rc != EZDB_OK) goto bulk_cleanup;
                bc = out_idx;
                /* Remap entry archive_ids to 0..N */
                own_archive_id_map_cap = db->header.file_count;
                for (uint32_t i = 0; i < ec; ++i) {
                    uint32_t orig_id = be[i].archive_id;
                    if (orig_id < own_archive_id_map_cap) {
                        be[i].archive_id = baim[orig_id];
                    }
                }
            }
        } else if (ec > 0 && baim) {
            /* Remap archive_ids from original to 0..N */
            for (uint32_t i = 0; i < ec; ++i) {
                uint32_t orig_id = be[i].archive_id;
                if (orig_id < bc) {
                    be[i].archive_id = baim[orig_id];
                }
            }
        }

        /* Sort entries by archive_id (required for parallel index build) */
        if (ec > 1 && bc > 0) {
            qsort(be, ec, sizeof(EzdbEntryRecord), cmp_entry_by_archive_id);
        }

        /* Build per-archive entry bases/counts for range-based stream access */
        uint32_t* archive_bases = NULL;
        uint32_t* archive_counts = NULL;
        if (ec > 0 && bc > 0) {
            archive_bases = (uint32_t*)calloc(bc, sizeof(uint32_t));
            archive_counts = (uint32_t*)calloc(bc, sizeof(uint32_t));
            if (!archive_bases || !archive_counts) { free(archive_bases); free(archive_counts); rc = EZDB_ERR_MEMORY; goto bulk_cleanup; }
            for (uint32_t i = 0; i < ec; ) {
                uint32_t aid = be[i].archive_id;
                uint32_t start = i;
                while (i < ec && be[i].archive_id == aid) ++i;
                if (aid < bc) {
                    archive_bases[aid] = start;
                    archive_counts[aid] = i - start;
                }
            }
        }

        /* Build to temp file via stream with parallel index threads */
        size_t db_path_len = strlen(db->path);
        char* tmp_path = (char*)malloc(db_path_len + 11u);
        if (!tmp_path) { free(archive_bases); free(archive_counts); rc = EZDB_ERR_MEMORY; goto bulk_cleanup; }
        sprintf(tmp_path, "%s.bulk.tmp", db->path);
        remove(tmp_path);

        if (ec > 0 && archive_bases && archive_counts) {
            MemoryStreamCtx mctx;
            mctx.entries = be;
            mctx.entry_count = ec;
            mctx.pos = 0;
            mctx.range_end = ec;
            mctx.archive_bases = archive_bases;
            mctx.archive_counts = archive_counts;
            mctx.archive_count = bc;

            EzdbEntryStream ez_stream;
            memset(&ez_stream, 0, sizeof(ez_stream));
            ez_stream.user_data = &mctx;
            ez_stream.reset = mem_stream_reset;
            ez_stream.reset_range = mem_stream_reset_range;
            ez_stream.next = mem_stream_next;
            ez_stream.open_range = mem_stream_open_range;
            ez_stream.close_range = mem_stream_close_range;

            EzdbBuildOptions opts;
            memset(&opts, 0, sizeof(opts));
            opts.flags = EZDB_BUILD_DEFAULT_FLAGS;
            opts.index_threads = (bc > 1 && ec > 100000) ? 6u : 1u;
            rc = ezdb_build_snapshot_stream_entries(ba, bc, &ez_stream, ec, tmp_path, &opts);
        } else {
            rc = ezdb_build_snapshot(ba, bc, NULL, 0, tmp_path);
        }

        free(archive_bases);
        free(archive_counts);

        /* Swap: close old, rename temp, reopen into same struct */
        if (rc == EZDB_OK) {
            if (db->fp) { fclose(db->fp); db->fp = NULL; }
            if (remove(db->path) != 0 || rename(tmp_path, db->path) != 0) {
                rc = EZDB_ERR_IO;
            }
        }
        if (rc == EZDB_OK) {
            char* reopen_path = ezdb_strdup_range(db->path, db_path_len);
            Ezdb* reopened = NULL;
            rc = ezdb_open(reopen_path, &reopened);
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

    bulk_cleanup:
        for (uint32_t i = 0; i < bc; ++i) { if (bap && bap[i]) free(bap[i]); }
        free(ba); free(bap); free(baim);
        bulk_free_entries(be, bep, berp, ec);
        return rc;
    }

    /* --- Non-bulk mode: existing behavior with path cache optimization --- */
    rc = append_delta_frame(db, EZDB_DELTA_BATCH_COMMIT);
    if (rc == EZDB_OK && db->batch_index_dirty) {
        rc = ezdb_rebuild_delta_entry_index(db);
    }
    if (rc == EZDB_OK) rc = ezdb_write_header(db);
    if (rc != EZDB_OK) {
        (void)restore_txn_snapshot(db);
        cleanup_txn_state(db);
        return rc;
    }
    cleanup_txn_state(db);
    return EZDB_OK;
}

int ezdb_rollback_write(Ezdb* db)
{
    if (!db) return EZDB_ERR_ARG;
    if (!db->write_txn_active) return EZDB_ERR_ARG;

    /* Bulk mode: free buffers, no snapshot to restore */
    if (db->bulk_write_mode) {
        ezdb_bulk_free(db);
        db->bulk_write_mode = 0;
        db->write_txn_active = 0;
        db->batch_index_deferred = 0;
        db->batch_index_dirty = 0;
        return EZDB_OK;
    }

    int rc = restore_txn_snapshot(db);
    cleanup_txn_state(db);
    if (db->fp) {
        if (db->format_v13) {
            int wrc = ezdb_write_header(db);
            if (wrc != EZDB_OK && rc == EZDB_OK) rc = wrc;
        }
        if (_fseeki64(db->fp, (__int64)db->header.reserved_offset, SEEK_SET) != 0) return EZDB_ERR_IO;
    }
    return rc;
}

static int archive_insert(Ezdb* db, const char* path, uint64_t size, uint64_t modified_time, uint32_t* out_id)
{
    if (!db || !path || !out_id) return EZDB_ERR_ARG;
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
    if (ezdb_ensure_active_bits_zero_extended(db, old_file_count, db->header.file_count) != EZDB_OK) {
        db->header.file_count = old_file_count;
        db->header.active_count = old_active_count;
        return EZDB_ERR_MEMORY;
    }
    ezdb_bitset_set(db->active_bits, id, 0);
    if (db->archive_first_entry_ids) db->archive_first_entry_ids[id] = UINT32_MAX;

    uint32_t path_len = (uint32_t)strlen(path);
    int rc = append_delta_disk(db, EZDB_DELTA_INSERT, id, path, size, modified_time, !db->write_txn_active);
    if (rc == EZDB_OK) rc = ezdb_append_delta_memory(db, EZDB_DELTA_INSERT, id, path, path_len, size, modified_time);
    if (rc == EZDB_OK) {
        ezdb_bitset_set(db->active_bits, id, 1);
        *out_id = id;
        return EZDB_OK;
    }
    db->header.file_count = old_file_count;
    db->header.active_count = old_active_count;
    db->header.delta_offset = old_delta_offset;
    db->header.delta_size = old_delta_size;
    db->header.reserved_offset = old_reserved_offset;
    (void)ezdb_resize_active_bits(db, old_file_count);
    return rc;
}

static int archive_update(Ezdb* db, uint32_t id, const char* path, uint64_t size, uint64_t modified_time)
{
    if (!db || !path) return EZDB_ERR_ARG;
    if (db->read_only) return EZDB_ERR_READ_ONLY;
    if (id >= db->header.file_count || !ezdb_bitset_get(db->active_bits, id)) return EZDB_ERR_NOT_FOUND;
    uint64_t old_delta_offset = db->header.delta_offset;
    uint64_t old_delta_size = db->header.delta_size;
    uint64_t old_reserved_offset = db->header.reserved_offset;
    uint32_t path_len = (uint32_t)strlen(path);
    int rc = append_delta_disk(db, EZDB_DELTA_UPDATE, id, path, size, modified_time, !db->write_txn_active);
    if (rc == EZDB_OK) rc = ezdb_append_delta_memory(db, EZDB_DELTA_UPDATE, id, path, path_len, size, modified_time);
    if (rc == EZDB_OK) {
        if (id < db->header.base_file_count) ezdb_bitset_set(db->covered_base_bits, id, 1);
        ezdb_bitset_set(db->active_bits, id, 1);
        return EZDB_OK;
    }
    db->header.delta_offset = old_delta_offset;
    db->header.delta_size = old_delta_size;
    db->header.reserved_offset = old_reserved_offset;
    return rc;
}

static int archive_delete(Ezdb* db, uint32_t id)
{
    if (!db) return EZDB_ERR_ARG;
    if (db->read_only) return EZDB_ERR_READ_ONLY;
    if (id >= db->header.file_count || !ezdb_bitset_get(db->active_bits, id)) return EZDB_ERR_NOT_FOUND;
    uint64_t old_active_count = db->header.active_count;
    uint64_t old_delta_offset = db->header.delta_offset;
    uint64_t old_delta_size = db->header.delta_size;
    uint64_t old_reserved_offset = db->header.reserved_offset;
    db->header.active_count -= 1u;
    int rc = append_delta_disk(db, EZDB_DELTA_DELETE, id, NULL, 0, 0, !db->write_txn_active);
    if (rc == EZDB_OK) rc = ezdb_append_delta_memory(db, EZDB_DELTA_DELETE, id, NULL, 0, 0, 0);
    if (rc == EZDB_OK) {
        ezdb_bitset_set(db->active_bits, id, 0);
        if (id < db->header.base_file_count) ezdb_bitset_set(db->covered_base_bits, id, 1);
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

    /* Bulk mode: buffer the archive record */
    if (db->bulk_write_mode) {
        uint32_t idx = db->bulk_archive_count;
        if (idx >= db->bulk_archive_cap) {
            uint32_t new_cap = db->bulk_archive_cap ? db->bulk_archive_cap * 2u : 64u;
            EzdbArchiveRecord* new_a = (EzdbArchiveRecord*)realloc(db->bulk_archives, sizeof(EzdbArchiveRecord) * (size_t)new_cap);
            char** new_p = (char**)realloc(db->bulk_archive_paths, sizeof(char*) * (size_t)new_cap);
            if (!new_a || !new_p) { free(new_a); free(new_p); return EZDB_ERR_MEMORY; }
            db->bulk_archives = new_a;
            db->bulk_archive_paths = new_p;
            db->bulk_archive_cap = new_cap;
        }
        db->bulk_archives[idx] = *record;
        db->bulk_archives[idx].file_path = ezdb_strdup_range(record->file_path, strlen(record->file_path));
        db->bulk_archive_paths[idx] = (char*)db->bulk_archives[idx].file_path; /* track for freeing */
        db->bulk_archive_count = idx + 1;
        *out_id = idx;
        return EZDB_OK;
    }

    for (uint32_t i = 0; i < db->header.base_file_count; ++i) {
        if (ezdb_bitset_get(db->active_bits, i) &&
            db->archive_meta[i].drive_letter == (unsigned char)record->drive_letter &&
            db->archive_meta[i].file_ref_number == record->file_ref_number) {
            int rc = archive_update(db, i, record->file_path, record->file_size, record->modified_time);
            if (rc == EZDB_OK) {
                db->archive_meta[i].usn = record->usn;
                *out_id = i;
            }
            return rc;
        }
    }
    return archive_insert(db, record->file_path, record->file_size, record->modified_time, out_id);
}

int ezdb_upsert_archives(Ezdb* db, const EzdbArchiveRecord* records, uint32_t count, uint32_t* out_ids)
{
    if (!db || (!records && count) || (!out_ids && count)) return EZDB_ERR_ARG;
    int rc = EZDB_OK;

    /* Bulk mode: buffer all archive records */
    if (db->bulk_write_mode) {
        for (uint32_t i = 0; i < count; ++i) {
            rc = ezdb_upsert_archive(db, &records[i], &out_ids[i]);
            if (rc != EZDB_OK) break;
        }
        /* Build archive ID map */
        if (rc == EZDB_OK && count > 0) {
            uint32_t max_id = 0;
            for (uint32_t i = 0; i < count; ++i) {
                if (out_ids[i] > max_id) max_id = out_ids[i];
            }
            uint32_t needed = max_id + 1;
            if (!db->bulk_archive_id_map || needed > db->bulk_archive_cap) {
                uint32_t new_cap = db->bulk_archive_cap ? db->bulk_archive_cap : 64u;
                while (new_cap < needed) new_cap *= 2u;
                uint32_t* new_map = (uint32_t*)realloc(db->bulk_archive_id_map, sizeof(uint32_t) * (size_t)new_cap);
                if (!new_map) return EZDB_ERR_MEMORY;
                memset(new_map + (db->bulk_archive_cap ? db->bulk_archive_cap : 0),
                       0xFF, sizeof(uint32_t) * (size_t)(new_cap - (db->bulk_archive_cap ? db->bulk_archive_cap : 0)));
                db->bulk_archive_id_map = new_map;
                db->bulk_archive_cap = new_cap;
            }
            for (uint32_t i = 0; i < count; ++i) {
                db->bulk_archive_id_map[out_ids[i]] = i;
            }
        }
        return rc;
    }

    /* Non-bulk mode: original logic */
    int own_txn = db->write_txn_active ? 0 : 1;
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
        if (ezdb_bitset_get(db->active_bits, i) &&
            db->archive_meta[i].drive_letter == (unsigned char)drive_letter &&
            db->archive_meta[i].file_ref_number == file_ref_number) {
            int own_txn = db->write_txn_active ? 0 : 1;
            int rc = EZDB_OK;
            if (own_txn) {
                rc = ezdb_begin_write(db, 0);
                if (rc != EZDB_OK) return rc;
            }
            rc = append_entry_delete_archive_frame(db, i, 0);
            if (rc == EZDB_OK) rc = archive_delete(db, i);
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
    if (archive_id >= db->header.file_count || !ezdb_bitset_get(db->active_bits, archive_id)) return EZDB_ERR_NOT_FOUND;
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
    if (db->bulk_write_mode) return EZDB_OK;
    return append_entry_delete_archive_frame(db, archive_id, !db->write_txn_active);
}

int ezdb_append_archive_entries(Ezdb* db, uint32_t archive_id, const EzdbEntryRecord* entries, uint32_t entry_count)
{
    if (!db || (!entries && entry_count)) return EZDB_ERR_ARG;
    if (db->read_only) return EZDB_ERR_READ_ONLY;

    /* Bulk mode: buffer entries in memory */
    if (db->bulk_write_mode) {
        if (!entry_count) return EZDB_OK;
        uint32_t old_count = db->bulk_entry_count;
        uint32_t new_count = old_count + entry_count;
        /* Grow arrays if needed */
        if (new_count > db->bulk_entry_cap) {
            uint32_t new_cap = db->bulk_entry_cap ? db->bulk_entry_cap * 2u : 4096u;
            while (new_cap < new_count) new_cap *= 2u;
            EzdbEntryRecord* new_e = (EzdbEntryRecord*)realloc(db->bulk_entries, sizeof(EzdbEntryRecord) * (size_t)new_cap);
            char** new_p = (char**)realloc(db->bulk_entry_paths, sizeof(char*) * (size_t)new_cap);
            void** new_r = (void**)realloc(db->bulk_entry_raw_paths, sizeof(void*) * (size_t)new_cap);
            if (!new_e || !new_p || !new_r) {
                free(new_e); free(new_p); free(new_r);
                return EZDB_ERR_MEMORY;
            }
            db->bulk_entries = new_e;
            db->bulk_entry_paths = new_p;
            db->bulk_entry_raw_paths = new_r;
            db->bulk_entry_cap = new_cap;
        }
        for (uint32_t i = 0; i < entry_count; ++i) {
            uint32_t idx = old_count + i;
            db->bulk_entries[idx] = entries[i];
            db->bulk_entries[idx].archive_id = archive_id;
            /* strdup paths so caller can free its copies */
            db->bulk_entry_paths[idx] = entries[i].entry_path
                ? ezdb_strdup_range(entries[i].entry_path, strlen(entries[i].entry_path))
                : NULL;
            db->bulk_entries[idx].entry_path = db->bulk_entry_paths[idx];
            if (entries[i].entry_raw_path && entries[i].entry_raw_path_len) {
                void* raw = malloc(entries[i].entry_raw_path_len);
                if (raw) memcpy(raw, entries[i].entry_raw_path, entries[i].entry_raw_path_len);
                db->bulk_entry_raw_paths[idx] = raw;
                db->bulk_entries[idx].entry_raw_path = raw;
            } else {
                db->bulk_entry_raw_paths[idx] = NULL;
                db->bulk_entries[idx].entry_raw_path = NULL;
            }
        }
        db->bulk_entry_count = new_count;
        return EZDB_OK;
    }

    if (archive_id >= db->header.file_count || !ezdb_bitset_get(db->active_bits, archive_id)) return EZDB_ERR_NOT_FOUND;
    if (!entry_count) {
        if (!db->write_txn_active) return ezdb_write_header(db);
        return EZDB_OK;
    }

    /* Preallocate entry arrays for the entire batch */
    uint64_t old_entry_count = db->header.entry_count;
    uint64_t new_entry_count = old_entry_count + entry_count;
    int rc = ezdb_resize_entry_arrays(db, new_entry_count);
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
        ezdb_bitset_set(db->active_entry_bits, id, 1);
        ezdb_bitset_set(db->delta_entry_bits, id, 1);
        ezdb_link_entry_to_archive(db, id, archive_id);

        /* Deferred gram index: skip during bulk import */
        if (path_len && entries[i].entry_path) {
            rc = ezdb_delta_entry_index_add_path(db, id, entries[i].entry_path);
            if (rc != EZDB_OK) break;
            /* Cache path for fast rebuild at commit */
            if (db->write_txn_active) {
                if (ezdb_path_cache_ensure(db, id + 1) == EZDB_OK) {
                    free(db->delta_entry_path_cache[id]); /* overwrite if exists */
                    db->delta_entry_path_cache[id] = ezdb_strdup_range(entries[i].entry_path, path_len);
                }
            }
        }
    }

    /* Single fwrite for the entire batch */
    if (rc == EZDB_OK && buf_cap > 0) {
        if (_fseeki64(db->fp, (__int64)append_base, SEEK_SET) != 0) rc = EZDB_ERR_IO;
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
        /* Rollback in-memory state on error: clear bitsets and archive links for partially-added entries */
        uint32_t added = (uint32_t)(db->header.entry_count - old_entry_count);
        for (uint32_t i = 0; i < added; ++i) {
            uint32_t id = (uint32_t)(old_entry_count + i);
            ezdb_bitset_set(db->active_entry_bits, id, 0);
            ezdb_bitset_set(db->delta_entry_bits, id, 0);
            db->entry_archive_ids[id] = 0;
            db->entry_path_offsets[id] = 0;
            db->entry_path_lens[id] = 0;
            memset(&db->delta_entry_refs[id], 0, sizeof(EzdbDeltaEntryRef));
        }
        /* Rebuild archive links since partial entries may have corrupted the linked list */
        ezdb_rebuild_archive_entry_links(db);
        db->header.entry_count = old_entry_count;
        db->header.active_entry_count -= entry_count;
        return rc;
    }
    if (!db->write_txn_active) rc = ezdb_write_header(db);
    return rc;
}

int ezdb_finish_replace_archive_entries(Ezdb* db, uint32_t archive_id)
{
    if (!db) return EZDB_ERR_ARG;
    if (archive_id >= db->header.file_count || !ezdb_bitset_get(db->active_bits, archive_id)) return EZDB_ERR_NOT_FOUND;
    if (db->bulk_write_mode) return EZDB_OK;
    return db->write_txn_active ? EZDB_OK : ezdb_write_header(db);
}

int ezdb_abort_replace_archive_entries(Ezdb* db, uint32_t archive_id)
{
    if (!db) return EZDB_ERR_ARG;
    if (archive_id >= db->header.file_count || !ezdb_bitset_get(db->active_bits, archive_id)) return EZDB_ERR_NOT_FOUND;
    return append_entry_delete_archive_frame(db, archive_id, !db->write_txn_active);
}

