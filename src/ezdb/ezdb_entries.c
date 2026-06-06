#include "ezdb_entries.h"
#include "ezdb_io.h"

#include <stdlib.h>
#include <string.h>

static int entry_bitset_get(const unsigned char* bits, uint32_t id)
{
    return bits && (bits[id >> 3u] & (unsigned char)(1u << (id & 7u)));
}

static int entries_ensure_capacity(void** data, size_t elem_size, uint32_t* capacity, uint32_t needed)
{
    if (!data || !capacity || !elem_size) return EZDB_ERR_ARG;
    if (needed <= *capacity) return EZDB_OK;
    uint32_t cap = *capacity ? *capacity : 16u;
    while (cap < needed) {
        if (cap > UINT32_MAX / 2u) {
            cap = needed;
            break;
        }
        cap *= 2u;
    }
    void* p = realloc(*data, elem_size * (size_t)cap);
    if (!p) return EZDB_ERR_MEMORY;
    *data = p;
    *capacity = cap;
    return EZDB_OK;
}

void ezdb_entries_page_cache_free(EzdbPageCacheEntry* cache, uint32_t count)
{
    if (!cache) return;
    for (uint32_t i = 0; i < count; ++i) {
        free(cache[i].data);
        memset(&cache[i], 0, sizeof(cache[i]));
    }
}

int ezdb_entries_section_writer_init(EzdbEntrySectionWriter* writer,
                                     FILE* core_out,
                                     FILE* detail_out,
                                     FILE* raw_out)
{
    if (!writer || !core_out || !detail_out || !raw_out) return EZDB_ERR_ARG;
    memset(writer, 0, sizeof(*writer));
    writer->core_out = core_out;
    int rc = ezdb_entries_paged_writer_init(&writer->detail_writer,
                                            detail_out,
                                            sizeof(EzdbDiskEntry) * EZDB_ENTRY_PAGE_SIZE);
    if (rc != EZDB_OK) return rc;
    rc = ezdb_entries_paged_writer_init(&writer->raw_writer, raw_out, EZDB_RAW_BLOB_PAGE_SIZE);
    if (rc != EZDB_OK) {
        ezdb_entries_paged_writer_free(&writer->detail_writer);
        memset(writer, 0, sizeof(*writer));
    }
    return rc;
}

void ezdb_entries_section_writer_free(EzdbEntrySectionWriter* writer)
{
    if (!writer) return;
    ezdb_entries_paged_writer_free(&writer->detail_writer);
    ezdb_entries_paged_writer_free(&writer->raw_writer);
    memset(writer, 0, sizeof(*writer));
}

int ezdb_entries_paged_writer_init(EzdbEntryPagedWriter* writer, FILE* out, uint32_t page_size)
{
    if (!writer || !out || !page_size) return EZDB_ERR_ARG;
    memset(writer, 0, sizeof(*writer));
    writer->out = out;
    writer->page_size = page_size;
    writer->page = (unsigned char*)malloc(page_size);
    if (!writer->page) return EZDB_ERR_MEMORY;
    return EZDB_OK;
}

void ezdb_entries_paged_writer_free(EzdbEntryPagedWriter* writer)
{
    if (!writer) return;
    free(writer->page);
    free(writer->pages);
    memset(writer, 0, sizeof(*writer));
}

static int entries_paged_writer_flush_page(EzdbEntryPagedWriter* writer)
{
    if (!writer || !writer->out) return EZDB_ERR_ARG;
    if (!writer->page_len) return EZDB_OK;
    if (entries_ensure_capacity((void**)&writer->pages,
                                sizeof(EzdbDiskPage),
                                &writer->page_cap,
                                writer->page_count + 1u) != EZDB_OK) {
        return EZDB_ERR_MEMORY;
    }
    unsigned char* payload = NULL;
    uint64_t payload_size = 0;
    uint32_t flags = 0;
    int rc = ezdb_io_maybe_compress_section(writer->page, writer->page_len, &payload, &payload_size, &flags);
    if (rc != EZDB_OK) return rc;
    EzdbDiskPage* page = &writer->pages[writer->page_count++];
    page->offset = writer->written;
    page->encoded_size = (uint32_t)payload_size;
    page->raw_size = writer->page_len;
    page->flags = flags;
    page->reserved = 0;
    if (payload_size && fwrite(payload, 1, (size_t)payload_size, writer->out) != (size_t)payload_size) rc = EZDB_ERR_IO;
    free(payload);
    if (rc != EZDB_OK) return rc;
    writer->written += payload_size;
    writer->page_len = 0;
    return EZDB_OK;
}

int ezdb_entries_paged_writer_write(EzdbEntryPagedWriter* writer, const void* data, uint32_t len)
{
    if (!writer || (!data && len)) return EZDB_ERR_ARG;
    const unsigned char* p = (const unsigned char*)data;
    while (len) {
        uint32_t room = writer->page_size - writer->page_len;
        uint32_t take = len < room ? len : room;
        memcpy(writer->page + writer->page_len, p, take);
        writer->page_len += take;
        writer->raw_size += take;
        p += take;
        len -= take;
        if (writer->page_len == writer->page_size) {
            int rc = entries_paged_writer_flush_page(writer);
            if (rc != EZDB_OK) return rc;
        }
    }
    return EZDB_OK;
}

int ezdb_entries_paged_writer_finish(EzdbEntryPagedWriter* writer)
{
    return entries_paged_writer_flush_page(writer);
}

int ezdb_entries_section_writer_add(EzdbEntrySectionWriter* writer,
                                    const EzdbEntryRecord* record,
                                    uint32_t final_archive_id)
{
    if (!writer || !writer->core_out || !record || !record->entry_path) return EZDB_ERR_ARG;
    uint32_t path_len = (uint32_t)strlen(record->entry_path);
    if (writer->raw_writer.raw_size > UINT32_MAX ||
        writer->raw_writer.raw_size + path_len + 1u > UINT32_MAX) {
        return EZDB_ERR_MEMORY;
    }
    uint32_t path_offset = (uint32_t)writer->raw_writer.raw_size;
    int rc = ezdb_entries_paged_writer_write(&writer->raw_writer, record->entry_path, path_len);
    if (rc == EZDB_OK) {
        unsigned char zero = 0;
        rc = ezdb_entries_paged_writer_write(&writer->raw_writer, &zero, 1u);
    }
    if (rc != EZDB_OK) return rc;

    EzdbDiskEntry disk_entry;
    memset(&disk_entry, 0, sizeof(disk_entry));
    disk_entry.archive_id = final_archive_id;
    disk_entry.entry_path_offset = path_offset;
    disk_entry.entry_path_len = path_len;
    disk_entry.compressed_size = record->compressed_size;
    disk_entry.original_size = record->original_size;
    disk_entry.modified_time = record->modified_time;
    if (record->entry_raw_path && record->entry_raw_path_len) {
        if (writer->raw_writer.raw_size > UINT32_MAX ||
            writer->raw_writer.raw_size + record->entry_raw_path_len > UINT32_MAX) {
            return EZDB_ERR_MEMORY;
        }
        disk_entry.raw_offset = (uint32_t)writer->raw_writer.raw_size;
        disk_entry.raw_len = record->entry_raw_path_len;
        rc = ezdb_entries_paged_writer_write(&writer->raw_writer,
                                             record->entry_raw_path,
                                             record->entry_raw_path_len);
        if (rc != EZDB_OK) return rc;
    }
    rc = ezdb_entries_paged_writer_write(&writer->detail_writer, &disk_entry, sizeof(disk_entry));
    if (rc != EZDB_OK) return rc;

    unsigned char core[EZDB_ENTRY_CORE_RECORD_SIZE];
    ezdb_entries_encode_core(&disk_entry, core);
    if (fwrite(core, 1, sizeof(core), writer->core_out) != sizeof(core)) return EZDB_ERR_IO;
    return EZDB_OK;
}

int ezdb_entries_section_writer_finish(EzdbEntrySectionWriter* writer)
{
    if (!writer) return EZDB_ERR_ARG;
    int rc = ezdb_entries_paged_writer_finish(&writer->detail_writer);
    if (rc == EZDB_OK) rc = ezdb_entries_paged_writer_finish(&writer->raw_writer);
    return rc;
}

int ezdb_entries_load_page_cached(FILE* fp,
                                  EzdbDiskPage* pages,
                                  uint32_t page_count,
                                  uint64_t section_offset,
                                  uint32_t page_id,
                                  EzdbPageCacheEntry* cache,
                                  uint32_t cache_count,
                                  uint64_t* cache_tick,
                                  const unsigned char** out_data,
                                  uint32_t* out_size)
{
    if (!fp || !pages || page_id >= page_count || !cache || !cache_count ||
        !cache_tick || !out_data || !out_size) {
        return EZDB_ERR_ARG;
    }
    for (uint32_t i = 0; i < cache_count; ++i) {
        if (cache[i].data && cache[i].page_id == page_id) {
            cache[i].tick = ++(*cache_tick);
            *out_data = cache[i].data;
            *out_size = cache[i].size;
            return EZDB_OK;
        }
    }
    uint32_t slot = UINT32_MAX;
    uint64_t oldest = UINT64_MAX;
    for (uint32_t i = 0; i < cache_count; ++i) {
        if (!cache[i].data) {
            slot = i;
            break;
        }
        if (cache[i].tick < oldest) {
            oldest = cache[i].tick;
            slot = i;
        }
    }
    if (slot == UINT32_MAX) return EZDB_ERR_MEMORY;
    EzdbDiskPage* page = &pages[page_id];
    unsigned char* data = NULL;
    int rc = read_section_payload(fp,
                                  section_offset + page->offset,
                                  page->encoded_size,
                                  page->raw_size,
                                  page->flags,
                                  &data);
    if (rc != EZDB_OK) return rc;
    free(cache[slot].data);
    cache[slot].data = data;
    cache[slot].page_id = page_id;
    cache[slot].size = page->raw_size;
    cache[slot].tick = ++(*cache_tick);
    *out_data = data;
    *out_size = page->raw_size;
    return EZDB_OK;
}

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
                                     unsigned char* out)
{
    if (!fp || !pages || (!out && len) || (uint64_t)offset + len > raw_size) return EZDB_ERR_FORMAT;
    uint32_t copied = 0;
    while (copied < len) {
        uint32_t absolute = offset + copied;
        uint32_t page_id = absolute / EZDB_RAW_BLOB_PAGE_SIZE;
        uint32_t page_pos = absolute % EZDB_RAW_BLOB_PAGE_SIZE;
        const unsigned char* page = NULL;
        uint32_t page_size = 0;
        int rc = ezdb_entries_load_page_cached(fp,
                                               pages,
                                               page_count,
                                               section_offset,
                                               page_id,
                                               cache,
                                               cache_count,
                                               cache_tick,
                                               &page,
                                               &page_size);
        if (rc != EZDB_OK) return rc;
        if (page_pos >= page_size) return EZDB_ERR_FORMAT;
        uint32_t chunk = page_size - page_pos;
        if (chunk > len - copied) chunk = len - copied;
        memcpy(out + copied, page + page_pos, chunk);
        copied += chunk;
    }
    return EZDB_OK;
}

int ezdb_entries_copy_delta_blob_range(FILE* fp, uint64_t offset, uint32_t len, unsigned char* out)
{
    if (!fp || (!out && len)) return EZDB_ERR_ARG;
    if (fseek(fp, (long)offset, SEEK_SET) != 0) return EZDB_ERR_IO;
    if (len && fread(out, 1, len, fp) != len) return EZDB_ERR_IO;
    return EZDB_OK;
}

int ezdb_entries_load_detail(const EzdbEntryDetailStore* store, uint32_t id, EzdbDiskEntry* out)
{
    if (!store || !out || id >= store->entry_count || !store->archive_ids ||
        !store->path_offsets || !store->path_lens) {
        return EZDB_ERR_ARG;
    }
    if (entry_bitset_get(store->delta_bits, id)) {
        if (!store->delta_refs) return EZDB_ERR_ARG;
        const EzdbDeltaEntryRef* ref = &store->delta_refs[id];
        memset(out, 0, sizeof(*out));
        out->archive_id = store->archive_ids[id];
        out->entry_path_offset = store->path_offsets[id];
        out->entry_path_len = store->path_lens[id];
        out->raw_offset = ref->raw_offset > UINT32_MAX ? UINT32_MAX : (uint32_t)ref->raw_offset;
        out->raw_len = ref->raw_len;
        out->compressed_size = ref->compressed_size;
        out->original_size = ref->original_size;
        out->modified_time = ref->modified_time;
        return EZDB_OK;
    }
    uint32_t page_id = id / EZDB_ENTRY_PAGE_SIZE;
    uint32_t index_in_page = id % EZDB_ENTRY_PAGE_SIZE;
    const unsigned char* page = NULL;
    uint32_t page_size = 0;
    int rc = ezdb_entries_load_page_cached(store->fp,
                                           store->pages,
                                           store->page_count,
                                           store->section_offset,
                                           page_id,
                                           store->cache,
                                           store->cache_count,
                                           store->cache_tick,
                                           &page,
                                           &page_size);
    if (rc != EZDB_OK) return rc;
    size_t offset = sizeof(EzdbDiskEntry) * (size_t)index_in_page;
    if (offset + sizeof(EzdbDiskEntry) > page_size) return EZDB_ERR_FORMAT;
    memcpy(out, page + offset, sizeof(*out));
    if (out->archive_id != store->archive_ids[id] ||
        out->entry_path_offset != store->path_offsets[id] ||
        out->entry_path_len != store->path_lens[id]) {
        return EZDB_ERR_FORMAT;
    }
    return EZDB_OK;
}

char* ezdb_entries_copy_path(const EzdbEntryPathStore* store, uint32_t id)
{
    if (!store || !store->fp || id >= store->entry_count || !store->path_offsets || !store->path_lens) {
        return NULL;
    }
    uint32_t len = store->path_lens[id];
    char* out = (char*)malloc((size_t)len + 1u);
    if (!out) return NULL;
    long saved_pos = ftell(store->fp);
    int rc = entry_bitset_get(store->delta_bits, id)
        ? (store->delta_refs
               ? ezdb_entries_copy_delta_blob_range(store->fp,
                                                    store->delta_refs[id].path_offset,
                                                    len,
                                                    (unsigned char*)out)
               : EZDB_ERR_ARG)
        : ezdb_entries_copy_raw_blob_range(store->fp,
                                           store->raw_blob_pages,
                                           store->raw_blob_page_count,
                                           store->raw_blob_section_offset,
                                           store->raw_blob_raw_size,
                                           store->raw_blob_cache,
                                           store->raw_blob_cache_count,
                                           store->cache_tick,
                                           store->path_offsets[id],
                                           len,
                                           (unsigned char*)out);
    if (saved_pos >= 0 && fseek(store->fp, saved_pos, SEEK_SET) != 0) rc = EZDB_ERR_IO;
    if (rc != EZDB_OK) {
        free(out);
        return NULL;
    }
    out[len] = '\0';
    return out;
}

void* ezdb_entries_copy_raw_path(const EzdbEntryPathStore* store, uint32_t id, const EzdbDiskEntry* detail)
{
    if (!store || !store->fp || !detail || id >= store->entry_count) return NULL;
    if (!detail->raw_len) return NULL;
    void* out = malloc(detail->raw_len);
    if (!out) return NULL;
    int rc = entry_bitset_get(store->delta_bits, id)
        ? (store->delta_refs
               ? ezdb_entries_copy_delta_blob_range(store->fp,
                                                    store->delta_refs[id].raw_offset,
                                                    detail->raw_len,
                                                    (unsigned char*)out)
               : EZDB_ERR_ARG)
        : ezdb_entries_copy_raw_blob_range(store->fp,
                                           store->raw_blob_pages,
                                           store->raw_blob_page_count,
                                           store->raw_blob_section_offset,
                                           store->raw_blob_raw_size,
                                           store->raw_blob_cache,
                                           store->raw_blob_cache_count,
                                           store->cache_tick,
                                           detail->raw_offset,
                                           detail->raw_len,
                                           (unsigned char*)out);
    if (rc != EZDB_OK) {
        free(out);
        return NULL;
    }
    return out;
}

static int array_entry_source_next(void* user_data, EzdbEntryRecord* out_record);
static int array_entry_source_reset(void* user_data);
static int array_entry_source_reset_range(void* user_data, uint32_t archive_begin, uint32_t archive_end);
static int array_entry_source_open_range(void* user_data,
                                         uint32_t archive_begin,
                                         uint32_t archive_end,
                                         EzdbEntrySource* out_source);
static void array_entry_source_close_range(EzdbEntrySource* source);

static void array_entry_source_bind(EzdbEntrySource* source, EzdbArrayEntrySource* array_source)
{
    if (!source || !array_source) return;
    memset(source, 0, sizeof(*source));
    source->user_data = array_source;
    source->reset = array_entry_source_reset;
    source->reset_range = array_entry_source_reset_range;
    source->next = array_entry_source_next;
    source->open_range = array_entry_source_open_range;
    source->close_range = array_entry_source_close_range;
}

static int array_entry_source_reset(void* user_data)
{
    EzdbArrayEntrySource* source = (EzdbArrayEntrySource*)user_data;
    if (!source) return EZDB_ERR_ARG;
    source->index = 0;
    source->archive_begin = 0;
    source->archive_end = UINT32_MAX;
    return EZDB_OK;
}

static int array_entry_source_reset_range(void* user_data, uint32_t archive_begin, uint32_t archive_end)
{
    EzdbArrayEntrySource* source = (EzdbArrayEntrySource*)user_data;
    if (!source || archive_begin > archive_end) return EZDB_ERR_ARG;
    source->index = 0;
    source->archive_begin = archive_begin;
    source->archive_end = archive_end;
    return EZDB_OK;
}

static int array_entry_source_open_range(void* user_data,
                                         uint32_t archive_begin,
                                         uint32_t archive_end,
                                         EzdbEntrySource* out_source)
{
    EzdbArrayEntrySource* source = (EzdbArrayEntrySource*)user_data;
    if (!source || !out_source || archive_begin > archive_end) return EZDB_ERR_ARG;
    EzdbArrayEntrySource* range_source = (EzdbArrayEntrySource*)malloc(sizeof(*range_source));
    if (!range_source) return EZDB_ERR_MEMORY;
    *range_source = *source;
    int rc = array_entry_source_reset_range(range_source, archive_begin, archive_end);
    if (rc != EZDB_OK) {
        free(range_source);
        return rc;
    }
    array_entry_source_bind(out_source, range_source);
    return EZDB_OK;
}

static void array_entry_source_close_range(EzdbEntrySource* source)
{
    if (!source) return;
    free(source->user_data);
    memset(source, 0, sizeof(*source));
}

static int array_entry_source_next(void* user_data, EzdbEntryRecord* out_record)
{
    EzdbArrayEntrySource* source = (EzdbArrayEntrySource*)user_data;
    if (!source || !out_record) return EZDB_ERR_ARG;
    while (source->index < source->count) {
        const EzdbEntryRecord* record = &source->entries[source->index++];
        if (record->archive_id >= source->archive_begin && record->archive_id < source->archive_end) {
            *out_record = *record;
            return EZDB_OK;
        }
    }
    return EZDB_ERR_NOT_FOUND;
}

void ezdb_entries_array_source_init(EzdbEntrySource* source,
                                    EzdbArrayEntrySource* array_source,
                                    const EzdbEntryRecord* entries,
                                    uint32_t count)
{
    if (!source || !array_source) return;
    memset(array_source, 0, sizeof(*array_source));
    array_source->entries = entries;
    array_source->count = count;
    array_source->archive_end = UINT32_MAX;
    array_entry_source_bind(source, array_source);
}

static int compact_entry_is_searchable(const EzdbCompactEntrySource* source, uint32_t entry_id)
{
    if (!source || entry_id >= source->detail_store.entry_count ||
        !entry_bitset_get(source->active_entry_bits, entry_id)) {
        return 0;
    }
    if (!source->detail_store.archive_ids) return 0;
    uint32_t archive_id = source->detail_store.archive_ids[entry_id];
    return archive_id < source->file_count && entry_bitset_get(source->active_archive_bits, archive_id);
}

void ezdb_entries_compact_source_clear_current(EzdbCompactEntrySource* source)
{
    if (!source) return;
    free(source->entry_path);
    free(source->raw_path);
    source->entry_path = NULL;
    source->raw_path = NULL;
}

int ezdb_entries_compact_source_reset(void* user_data)
{
    EzdbCompactEntrySource* source = (EzdbCompactEntrySource*)user_data;
    if (!source) return EZDB_ERR_ARG;
    ezdb_entries_compact_source_clear_current(source);
    source->next_entry_id = 0;
    return EZDB_OK;
}

int ezdb_entries_compact_source_next(void* user_data, EzdbEntryRecord* out_record)
{
    EzdbCompactEntrySource* source = (EzdbCompactEntrySource*)user_data;
    if (!source || !out_record || !source->archive_id_map) return EZDB_ERR_ARG;
    ezdb_entries_compact_source_clear_current(source);
    while (source->next_entry_id < source->detail_store.entry_count) {
        uint32_t id = source->next_entry_id++;
        if (!compact_entry_is_searchable(source, id)) continue;
        EzdbDiskEntry detail;
        int rc = ezdb_entries_load_detail(&source->detail_store, id, &detail);
        if (rc != EZDB_OK) return rc;
        if (detail.archive_id >= source->file_count ||
            source->archive_id_map[detail.archive_id] == UINT32_MAX) {
            continue;
        }
        source->entry_path = ezdb_entries_copy_path(&source->path_store, id);
        if (!source->entry_path) return EZDB_ERR_MEMORY;
        memset(out_record, 0, sizeof(*out_record));
        out_record->archive_id = source->archive_id_map[detail.archive_id];
        out_record->entry_path = source->entry_path;
        out_record->compressed_size = detail.compressed_size;
        out_record->original_size = detail.original_size;
        out_record->modified_time = detail.modified_time;
        if (detail.raw_len) {
            source->raw_path = ezdb_entries_copy_raw_path(&source->path_store, id, &detail);
            if (!source->raw_path) return EZDB_ERR_MEMORY;
            out_record->entry_raw_path = source->raw_path;
            out_record->entry_raw_path_len = detail.raw_len;
        }
        return EZDB_OK;
    }
    return EZDB_ERR_NOT_FOUND;
}

void ezdb_entries_encode_core(const EzdbDiskEntry* entry, unsigned char out[EZDB_ENTRY_CORE_RECORD_SIZE])
{
    uint32_t archive_id = entry ? entry->archive_id : 0;
    uint32_t path_offset = entry ? entry->entry_path_offset : 0;
    uint32_t path_len = entry ? entry->entry_path_len : 0;

    out[0] = (unsigned char)(archive_id & 0xffu);
    out[1] = (unsigned char)((archive_id >> 8) & 0xffu);
    out[2] = (unsigned char)((archive_id >> 16) & 0xffu);
    out[3] = (unsigned char)((archive_id >> 24) & 0xffu);
    out[4] = (unsigned char)(path_offset & 0xffu);
    out[5] = (unsigned char)((path_offset >> 8) & 0xffu);
    out[6] = (unsigned char)((path_offset >> 16) & 0xffu);
    out[7] = (unsigned char)((path_offset >> 24) & 0xffu);
    out[8] = (unsigned char)(path_len & 0xffu);
    out[9] = (unsigned char)((path_len >> 8) & 0xffu);
    out[10] = (unsigned char)((path_len >> 16) & 0xffu);
    out[11] = (unsigned char)((path_len >> 24) & 0xffu);
}

void ezdb_entries_decode_core(const unsigned char raw[EZDB_ENTRY_CORE_RECORD_SIZE], EzdbDiskEntry* out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!raw) return;
    out->archive_id = (uint32_t)raw[0] |
                      ((uint32_t)raw[1] << 8u) |
                      ((uint32_t)raw[2] << 16u) |
                      ((uint32_t)raw[3] << 24u);
    out->entry_path_offset = (uint32_t)raw[4] |
                             ((uint32_t)raw[5] << 8u) |
                             ((uint32_t)raw[6] << 16u) |
                             ((uint32_t)raw[7] << 24u);
    out->entry_path_len = (uint32_t)raw[8] |
                          ((uint32_t)raw[9] << 8u) |
                          ((uint32_t)raw[10] << 16u) |
                          ((uint32_t)raw[11] << 24u);
}
