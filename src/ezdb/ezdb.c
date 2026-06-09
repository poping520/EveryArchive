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
#include "ezdb_build.h"
#include "ezdb_core_internal.h"
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

static int append_blob(unsigned char** data, uint32_t* size, uint32_t* cap, const void* bytes, uint32_t len, uint32_t extra_nul, uint32_t* out_offset);

static uint64_t file_size_of(FILE* fp)
{
    __int64 old_pos = _ftelli64(fp);
    if (_fseeki64(fp, 0, SEEK_END) != 0) return 0;
    __int64 size = _ftelli64(fp);
    _fseeki64(fp, old_pos, SEEK_SET);
    return size < 0 ? 0 : (uint64_t)size;
}

double ezdb_now_ms(void)
{
    return (double)clock() * 1000.0 / (double)CLOCKS_PER_SEC;
}

uint32_t ezdb_fnv1a_bytes(const char* text, size_t len)
{
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < len; ++i) {
        hash ^= (unsigned char)text[i];
        hash *= 16777619u;
    }
    return hash;
}

static double ezdb_peak_working_set_mb(void)
{
    PROCESS_MEMORY_COUNTERS_EX counters;
    memset(&counters, 0, sizeof(counters));
    counters.cb = sizeof(counters);
    if (!GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&counters, sizeof(counters))) return 0.0;
    return (double)counters.PeakWorkingSetSize / 1024.0 / 1024.0;
}

char* ezdb_strdup_range(const char* text, size_t len)
{
    char* out = (char*)malloc(len + 1);
    if (!out) return NULL;
    memcpy(out, text, len);
    out[len] = '\0';
    return out;
}

int ezdb_ensure_capacity(void** data, size_t elem_size, uint32_t* capacity, uint32_t needed)
{
    if (*capacity >= needed) return EZDB_OK;
    uint32_t next = *capacity ? *capacity : 1024;
    while (next < needed) {
        if (next > UINT32_MAX / 2u) { next = needed; break; }
        next *= 2u;
    }
    void* new_data = realloc(*data, elem_size * (size_t)next);
    if (!new_data) return EZDB_ERR_MEMORY;
    *data = new_data;
    *capacity = next;
    return EZDB_OK;
}

uint32_t ezdb_next_pow2_u32(uint32_t value)
{
    uint32_t out = 1;
    while (out < value && out < 0x80000000u) out <<= 1u;
    return out ? out : 1u;
}

void ezdb_bitset_set(unsigned char* bits, uint32_t id, int value)
{
    unsigned char mask = (unsigned char)(1u << (id & 7u));
    if (value) bits[id >> 3u] |= mask;
    else bits[id >> 3u] &= (unsigned char)~mask;
}

int ezdb_bitset_get(const unsigned char* bits, uint32_t id)
{
    return bits && (bits[id >> 3u] & (unsigned char)(1u << (id & 7u)));
}

int ezdb_resize_entry_arrays(Ezdb* db, uint64_t entry_count)
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
    int rc = ezdb_resize_entry_arrays(db, new_count);
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

int ezdb_resize_active_bits(Ezdb* db, uint64_t file_count)
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

void ezdb_link_entry_to_archive(Ezdb* db, uint32_t entry_id, uint32_t archive_id)
{
    if (!db || !db->archive_first_entry_ids || !db->entry_next_in_archive) return;
    if (entry_id >= db->header.entry_count || archive_id >= db->header.file_count) return;
    db->entry_next_in_archive[entry_id] = db->archive_first_entry_ids[archive_id];
    db->archive_first_entry_ids[archive_id] = entry_id;
}

void ezdb_rebuild_archive_entry_links(Ezdb* db)
{
    clear_archive_entry_links(db);
    if (!db || !db->active_entry_bits) return;
    for (uint32_t e = 0; e < db->header.entry_count; ++e) {
        uint32_t archive_id = db->entry_archive_ids[e];
        if (archive_id < db->header.file_count && ezdb_bitset_get(db->active_entry_bits, e)) {
            ezdb_link_entry_to_archive(db, e, archive_id);
        }
    }
}

int ezdb_deactivate_entries_for_archive(Ezdb* db, uint32_t archive_id)
{
    if (!db || archive_id >= db->header.file_count) return EZDB_ERR_NOT_FOUND;
    if (!db->archive_first_entry_ids || !db->entry_next_in_archive) return EZDB_OK;
    EzdbEntryPathStore path_store = ezdb_entry_path_store(db);
    uint32_t e = db->archive_first_entry_ids[archive_id];
    db->archive_first_entry_ids[archive_id] = UINT32_MAX;
    while (e != UINT32_MAX && e < db->header.entry_count) {
        uint32_t next = db->entry_next_in_archive[e];
        db->entry_next_in_archive[e] = UINT32_MAX;
        if (db->entry_archive_ids[e] == archive_id && ezdb_bitset_get(db->active_entry_bits, e)) {
            if (ezdb_bitset_get(db->delta_entry_bits, e)) {
                char* entry_path = ezdb_entries_copy_path(&path_store, e);
                if (entry_path) {
                    int rc = ezdb_delta_entry_index_remove_path(db, e, entry_path);
                    free(entry_path);
                    if (rc != EZDB_OK) return rc;
                }
            }
            ezdb_bitset_set(db->active_entry_bits, e, 0);
            if (db->header.active_entry_count) db->header.active_entry_count -= 1u;
        }
        e = next;
    }
    return EZDB_OK;
}

int ezdb_ensure_active_bits_zero_extended(Ezdb* db, uint64_t old_file_count, uint64_t new_file_count)
{
    size_t old_bytes = ((size_t)old_file_count + 7u) / 8u;
    size_t new_bytes = ((size_t)new_file_count + 7u) / 8u;
    int rc = ezdb_resize_active_bits(db, new_file_count);
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

int ezdb_delta_entry_index_ensure(Ezdb* db)
{
    if (!db) return EZDB_ERR_ARG;
    if (db->delta_entry_index_ready) return EZDB_OK;
    int rc = ezdb_postings_builder_init(&db->delta_entry_index, 4096u);
    if (rc != EZDB_OK) return rc;
    db->delta_entry_index_ready = 1;
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
        if (ezdb_ensure_capacity((void**)&db->file_size_overflow_ids, sizeof(uint32_t), &db->file_size_overflow_id_cap, db->file_size_overflow_count + 1) != EZDB_OK ||
            ezdb_ensure_capacity((void**)&db->file_size_overflow_values, sizeof(uint64_t), &db->file_size_overflow_value_cap, db->file_size_overflow_count + 1) != EZDB_OK) {
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
        if (ezdb_ensure_capacity((void**)&db->file_mtime_overflow_ids, sizeof(uint32_t), &db->file_mtime_overflow_id_cap, db->file_mtime_overflow_count + 1) != EZDB_OK ||
            ezdb_ensure_capacity((void**)&db->file_mtime_overflow_values, sizeof(uint64_t), &db->file_mtime_overflow_value_cap, db->file_mtime_overflow_count + 1) != EZDB_OK) {
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

int ezdb_build_resolve_options(const char* output_ezdb, const EzdbBuildOptions* options, EzdbBuildOptionsResolved* out)
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

EzdbEntryDetailStore ezdb_entry_detail_store(Ezdb* db)
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

EzdbEntryPathStore ezdb_entry_path_store(Ezdb* db)
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

int ezdb_write_header(Ezdb* db)
{
    if (db && db->format_v13) return ezdb_write_v13_db_header(db);
    if (fseek(db->fp, 0, SEEK_SET) != 0 || fwrite(&db->header, sizeof(db->header), 1, db->fp) != 1) {
        return EZDB_ERR_IO;
    }
    return flush_file(db->fp);
}

static const char* dir_name(Ezdb* db, const EzdbDiskDir* d)
{
    return db->strings + d->name_offset;
}

const char* ezdb_file_name_by_id(Ezdb* db, uint32_t id)
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

uint64_t ezdb_file_size_by_id(Ezdb* db, uint32_t id)
{
    return lookup_u64_overflow(id,
                               db->file_sizes32[id],
                               db->file_size_overflow_ids,
                               db->file_size_overflow_values,
                               db->file_size_overflow_count);
}

uint64_t ezdb_file_modified_time_by_id(Ezdb* db, uint32_t id)
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
        if (ezdb_ensure_capacity((void**)&stack, sizeof(uint32_t), &stack_cap, stack_count + 1) != EZDB_OK) {
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

int ezdb_build_result_path(Ezdb* db, uint32_t id, char** out_path)
{
    if (!db || !out_path) return EZDB_ERR_ARG;
    if (id >= db->header.file_count) return EZDB_ERR_NOT_FOUND;
    if (!ezdb_bitset_get(db->active_bits, id)) return EZDB_ERR_NOT_FOUND;
    *out_path = NULL;
    EzdbDeltaRecord* delta = ezdb_find_delta_record(db, id);
    if (delta) {
        if (delta->type == EZDB_DELTA_DELETE) return EZDB_ERR_NOT_FOUND;
        *out_path = ezdb_strdup_range(delta->path, delta->path_len);
        return *out_path ? EZDB_OK : EZDB_ERR_MEMORY;
    }
    if (id >= db->header.base_file_count) return EZDB_ERR_NOT_FOUND;
    char* dir_path = NULL;
    uint32_t dir_len = 0;
    uint32_t name_len = db->file_name_lens[id];
    const char* name = ezdb_file_name_by_id(db, id);
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
    *out_path = path;
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

static int ezdb_write_v13_db_header(Ezdb* db)
{
    if (!db || !db->fp) return EZDB_ERR_ARG;
    uint64_t table_offset = db->header.delta_offset
        ? db->header.delta_offset + db->header.delta_size
        : db->v13_section_table_offset;
    if (!table_offset) table_offset = db->header.reserved_offset;
    if (_fseeki64(db->fp, (__int64)table_offset, SEEK_SET) != 0) return EZDB_ERR_IO;

    EzdbSectionDesc sections[EZDB_SECTION_METADATA];
    uint32_t section_count = 0;
    int rc = ezdb_format_build_v13_sections_from_header(&db->header, sections, EZDB_SECTION_METADATA, &section_count);
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

    rc = ezdb_format_write_v13_disk_header(db->fp, &db->header, section_count, table_offset);
    if (rc != EZDB_OK) return rc;
    return flush_file(db->fp);
}

uint64_t ezdb_delta_append_offset(Ezdb* db)
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
    __int64 current = _ftelli64(fp);
    if (current < 0) return EZDB_ERR_IO;
    if (_fseeki64(fp, 0, SEEK_END) != 0) return EZDB_ERR_IO;
    __int64 end = _ftelli64(fp);
    if (end < 0) return EZDB_ERR_IO;
    if (_fseeki64(fp, current, SEEK_SET) != 0) return EZDB_ERR_IO;
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

int ezdb_build_write_archive_base_core(const EzdbArchiveRecord* archives,
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

    EzdbArchiveBuildTree tree;
    EzdbBuildArchiveBaseStats archive_base_stats;
    EzdbBuildArchivePostingsResult archive_postings;
    int rc = EZDB_OK;

    memset(&tree, 0, sizeof(tree));
    memset(&archive_base_stats, 0, sizeof(archive_base_stats));
    memset(&archive_postings, 0, sizeof(archive_postings));
    rc = ezdb_build_archive_tree_init(&tree);
    if (rc != EZDB_OK) return rc;

    double stage_start_ms = ezdb_now_ms();
    rc = ezdb_build_archive_tree_add_archives(&tree, archives, archive_count);
    build_tree_ms = ezdb_now_ms() - stage_start_ms;

    if (rc == EZDB_OK) {
        stage_start_ms = ezdb_now_ms();
        rc = ezdb_build_archive_tree_assign(&tree, original_to_final);
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
            header.file_count = tree.file_count;
            header.active_count = tree.file_count;
            header.base_file_count = tree.file_count;
            header.dir_count = tree.dir_count;
            EzdbV13Header disk_header_placeholder;
            memset(&disk_header_placeholder, 0, sizeof(disk_header_placeholder));
            if (fwrite(&disk_header_placeholder, sizeof(disk_header_placeholder), 1, out) != 1) rc = EZDB_ERR_IO;

            if (rc == EZDB_OK) {
                rc = ezdb_build_write_archive_base_sections(out, &tree, &header, &archive_base_stats);
                write_base_ms = archive_base_stats.write_ms;
            }

            header.postings_offset = (uint64_t)_ftelli64(out);
            if (rc == EZDB_OK) rc = ezdb_build_write_archive_postings(out, &tree, &archive_postings);
            if (rc == EZDB_OK) {
                file_index_ms = archive_postings.file_index_ms;
                dir_index_ms = archive_postings.dir_index_ms;
            }
            header.postings_size = archive_postings.file_postings_size + archive_postings.dir_postings_size;

            if (rc == EZDB_OK) {
                if (entry_source) {
                    rc = ezdb_write_entries_from_source(out, &header, entry_source, entry_count, archive_count, original_to_final, options);
                } else {
                    rc = ezdb_write_entries(out, &header, entries, entry_count, archive_count, original_to_final, options);
                }
            }

            header.file_index_offset = (uint64_t)_ftelli64(out);
            header.file_index_count = archive_postings.file_index_count;
            if (rc == EZDB_OK && archive_postings.file_index_count &&
                fwrite(archive_postings.file_index,
                       sizeof(EzdbDiskIndex),
                       archive_postings.file_index_count,
                       out) != archive_postings.file_index_count) {
                rc = EZDB_ERR_IO;
            }

            for (uint32_t i = 0; i < archive_postings.dir_index_count; ++i) {
                archive_postings.dir_index[i].offset += archive_postings.file_postings_size;
            }
            header.dir_index_offset = (uint64_t)_ftelli64(out);
            header.dir_index_count = archive_postings.dir_index_count;
            if (rc == EZDB_OK && archive_postings.dir_index_count &&
                fwrite(archive_postings.dir_index,
                       sizeof(EzdbDiskIndex),
                       archive_postings.dir_index_count,
                       out) != archive_postings.dir_index_count) {
                rc = EZDB_ERR_IO;
            }

            if (rc == EZDB_OK) rc = ezdb_format_write_v13_header_and_section_table(out, &header, NULL, NULL);
            fclose(out);
        }
    }

    ezdb_build_archive_postings_result_free(&archive_postings);
    ezdb_build_archive_tree_free(&tree);
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

static int append_blob(unsigned char** data, uint32_t* size, uint32_t* cap, const void* bytes, uint32_t len, uint32_t extra_nul, uint32_t* out_offset)
{
    if (ezdb_ensure_capacity((void**)data, 1, cap, *size + len + extra_nul) != EZDB_OK) return EZDB_ERR_MEMORY;
    *out_offset = *size;
    if (len) memcpy(*data + *size, bytes, len);
    *size += len;
    if (extra_nul) (*data)[(*size)++] = '\0';
    return EZDB_OK;
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
        if (_fseeki64(fp, (__int64)db->header.entry_detail_index_offset, SEEK_SET) != 0 ||
            fread(db->entry_detail_pages, sizeof(EzdbDiskPage), (size_t)db->header.entry_detail_page_count, fp) != (size_t)db->header.entry_detail_page_count) {
            rc = EZDB_ERR_IO;
        }
    }
    if (rc == EZDB_OK && db->header.raw_blob_index_offset && db->header.raw_blob_page_count) {
        if (_fseeki64(fp, (__int64)db->header.raw_blob_index_offset, SEEK_SET) != 0 ||
            fread(db->raw_blob_pages, sizeof(EzdbDiskPage), (size_t)db->header.raw_blob_page_count, fp) != (size_t)db->header.raw_blob_page_count) {
            rc = EZDB_ERR_IO;
        }
    }
    if (rc != EZDB_OK) {
        ezdb_close(db);
        return rc;
    }
    if (_fseeki64(fp, (__int64)db->header.file_index_offset, SEEK_SET) != 0 ||
        fread(db->file_index, sizeof(EzdbDiskIndex), (size_t)db->header.file_index_count, fp) != (size_t)db->header.file_index_count ||
        _fseeki64(fp, (__int64)db->header.dir_index_offset, SEEK_SET) != 0 ||
        fread(db->dir_index, sizeof(EzdbDiskIndex), (size_t)db->header.dir_index_count, fp) != (size_t)db->header.dir_index_count) {
        ezdb_close(db);
        return EZDB_ERR_IO;
    }
    if (db->header.entry_index_count) {
        if (_fseeki64(fp, (__int64)db->header.entry_index_offset, SEEK_SET) != 0 ||
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
    ezdb_rebuild_archive_entry_links(db);
    rc = ezdb_replay_delta_log(db);
    if (rc != EZDB_OK) {
        ezdb_close(db);
        return rc;
    }
    *out_db = db;
    return EZDB_OK;
}

void ezdb_release_members(Ezdb* db, int free_path)
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
    /* Bulk mode buffers */
    if (db->bulk_archive_paths) {
        for (uint32_t i = 0; i < db->bulk_archive_count; ++i) free(db->bulk_archive_paths[i]);
    }
    free(db->bulk_archives);
    free(db->bulk_archive_paths);
    free(db->bulk_archive_id_map);
    if (db->bulk_entry_paths) {
        for (uint32_t i = 0; i < db->bulk_entry_count; ++i) free(db->bulk_entry_paths[i]);
    }
    if (db->bulk_entry_raw_paths) {
        for (uint32_t i = 0; i < db->bulk_entry_count; ++i) free(db->bulk_entry_raw_paths[i]);
    }
    free(db->bulk_entries);
    free(db->bulk_entry_paths);
    free(db->bulk_entry_raw_paths);
    /* Path cache */
    if (db->delta_entry_path_cache) {
        for (uint32_t i = 0; i < db->delta_entry_path_cache_cap; ++i) free(db->delta_entry_path_cache[i]);
    }
    free(db->delta_entry_path_cache);
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

uint32_t ezdb_entry_count(Ezdb* db)
{
    return db ? (uint32_t)db->header.entry_count : 0;
}

static uint32_t ezdb_compute_active_entry_count(Ezdb* db)
{
    if (!db) return 0;
    uint32_t count = 0;
    for (uint32_t i = 0; i < db->header.entry_count; ++i) {
        if (ezdb_bitset_get(db->active_entry_bits, i) &&
            db->entry_archive_ids[i] < db->header.file_count &&
            ezdb_bitset_get(db->active_bits, db->entry_archive_ids[i])) {
            ++count;
        }
    }
    return count;
}

uint32_t ezdb_active_entry_count(Ezdb* db)
{
    return db ? (uint32_t)db->header.active_entry_count : 0;
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

int ezdb_get_archive(Ezdb* db, uint32_t id, EzdbArchiveResult* out_result)
{
    if (!db || !out_result) return EZDB_ERR_ARG;
    char* path = NULL;
    int rc = ezdb_build_result_path(db, id, &path);
    if (rc != EZDB_OK) return rc;
    memset(out_result, 0, sizeof(*out_result));
    out_result->id = id;
    out_result->file_path = path;
    out_result->file_size = ezdb_file_size_by_id(db, id);
    out_result->modified_time = ezdb_file_modified_time_by_id(db, id);
    if (id < db->header.base_file_count && db->archive_meta) {
        out_result->drive_letter = (char)db->archive_meta[id].drive_letter;
        out_result->file_ref_number = db->archive_meta[id].file_ref_number;
        out_result->usn = db->archive_meta[id].usn;
    }
    return EZDB_OK;
}

void ezdb_free_entry_result(EzdbEntryResult* result);

int ezdb_get_entry(Ezdb* db, uint32_t id, EzdbEntryResult* out_result)
{
    if (!db || !out_result) return EZDB_ERR_ARG;
    if (id >= db->header.entry_count || !ezdb_bitset_get(db->active_entry_bits, id)) return EZDB_ERR_NOT_FOUND;
    EzdbDiskEntry detail;
    EzdbEntryDetailStore store = ezdb_entry_detail_store(db);
    int rc = ezdb_entries_load_detail(&store, id, &detail);
    if (rc != EZDB_OK) return rc;
    if (detail.archive_id >= db->header.file_count || !ezdb_bitset_get(db->active_bits, detail.archive_id)) return EZDB_ERR_NOT_FOUND;
    EzdbEntryPathStore path_store = ezdb_entry_path_store(db);
    char* entry_path = ezdb_entries_copy_path(&path_store, id);
    if (!entry_path) return EZDB_ERR_MEMORY;

    memset(out_result, 0, sizeof(*out_result));
    out_result->id = id;
    out_result->archive_id = detail.archive_id;
    out_result->entry_path = entry_path;
    char* archive_path = NULL;
    rc = ezdb_build_result_path(db, detail.archive_id, &archive_path);
    if (rc != EZDB_OK) {
        ezdb_free_entry_result(out_result);
        return rc;
    }
    out_result->archive_path = archive_path;
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

int ezdb_get_archive_by_ref(Ezdb* db, char drive_letter, uint64_t file_ref_number, EzdbArchiveResult* out_result)
{
    if (!db || !out_result) return EZDB_ERR_ARG;
    for (uint32_t i = 0; i < db->header.base_file_count; ++i) {
        if (ezdb_bitset_get(db->active_bits, i) &&
            db->archive_meta &&
            db->archive_meta[i].drive_letter == (unsigned char)drive_letter &&
            db->archive_meta[i].file_ref_number == file_ref_number) {
            return ezdb_get_archive(db, i, out_result);
        }
    }
    return EZDB_ERR_NOT_FOUND;
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

    /* If no delta entries exist, database is already compact */
    {
        int has_delta = 0;
        for (uint32_t e = 0; e < db->header.entry_count && !has_delta; ++e) {
            if (ezdb_bitset_get(db->delta_entry_bits, e)) has_delta = 1;
        }
        if (!has_delta && db->header.delta_size == 0) return EZDB_OK;
    }

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
        char* path = NULL;
        rc = ezdb_build_result_path(db, i, &path);
        if (rc == EZDB_ERR_NOT_FOUND) {
            rc = EZDB_OK;
            continue;
        }
        if (rc != EZDB_OK) break;
        archive_id_map[i] = out_archive_count;
        archive_paths[out_archive_count] = path;
        archives[out_archive_count].file_path = archive_paths[out_archive_count];
        archives[out_archive_count].file_size = ezdb_file_size_by_id(db, i);
        archives[out_archive_count].modified_time = ezdb_file_modified_time_by_id(db, i);
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
    compact_source.detail_store = ezdb_entry_detail_store(db);
    compact_source.path_store = ezdb_entry_path_store(db);
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
        rc = ezdb_build_resolve_options(tmp_path, NULL, &options);
        if (rc == EZDB_OK) {
            rc = ezdb_build_write_archive_base_core(archives,
                                                    out_archive_count,
                                                    NULL,
                                                    active_entries,
                                                    tmp_path,
                                                    build_archive_id_map,
                                                    active_entries ? &source : NULL,
                                                    &options);
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

