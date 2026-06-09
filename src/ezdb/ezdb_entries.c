#include "ezdb_entries.h"
#include "ezdb_core_internal.h"
#include "ezdb_io.h"

#include <direct.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int entries_ensure_directory_exists(const char* path)
{
    if (!path || !path[0]) return EZDB_ERR_ARG;
    if (_mkdir(path) == 0 || errno == EEXIST) return EZDB_OK;
    return EZDB_ERR_IO;
}

static int entries_copy_file_payload(FILE* dst, const char* src_path, uint64_t* out_written)
{
    if (!dst || !src_path || !out_written) return EZDB_ERR_ARG;
    *out_written = 0;
    FILE* src = fopen(src_path, "rb");
    if (!src) return EZDB_ERR_IO;
    unsigned char* buf = (unsigned char*)malloc(1024u * 1024u);
    if (!buf) {
        fclose(src);
        return EZDB_ERR_MEMORY;
    }
    int rc = EZDB_OK;
    for (;;) {
        size_t n = fread(buf, 1, 1024u * 1024u, src);
        if (n) {
            if (fwrite(buf, 1, n, dst) != n) {
                rc = EZDB_ERR_IO;
                break;
            }
            *out_written += (uint64_t)n;
        }
        if (n < 1024u * 1024u) {
            if (ferror(src)) rc = EZDB_ERR_IO;
            break;
        }
    }
    free(buf);
    if (fclose(src) != 0 && rc == EZDB_OK) rc = EZDB_ERR_IO;
    return rc;
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
    if (ezdb_ensure_capacity((void**)&writer->pages,
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

int ezdb_entries_section_build_begin(EzdbEntrySectionBuild* build, const char* temp_dir)
{
    if (!build || !temp_dir) return EZDB_ERR_ARG;
    memset(build, 0, sizeof(*build));
    if (snprintf(build->core_path, sizeof(build->core_path), "%s\\entry_core.tmp", temp_dir) >= (int)sizeof(build->core_path) ||
        snprintf(build->detail_path, sizeof(build->detail_path), "%s\\entry_detail.tmp", temp_dir) >= (int)sizeof(build->detail_path) ||
        snprintf(build->raw_path, sizeof(build->raw_path), "%s\\entry_raw.tmp", temp_dir) >= (int)sizeof(build->raw_path)) {
        return EZDB_ERR_ARG;
    }
    int rc = entries_ensure_directory_exists(temp_dir);
    if (rc != EZDB_OK) return rc;
    build->core_fp = fopen(build->core_path, "wb");
    build->detail_fp = fopen(build->detail_path, "wb");
    build->raw_fp = fopen(build->raw_path, "wb");
    if (!build->core_fp || !build->detail_fp || !build->raw_fp) {
        ezdb_entries_section_build_free(build);
        return EZDB_ERR_IO;
    }
    rc = ezdb_entries_section_writer_init(&build->writer, build->core_fp, build->detail_fp, build->raw_fp);
    if (rc != EZDB_OK) {
        ezdb_entries_section_build_free(build);
        return rc;
    }
    build->writer_ready = 1;
    return EZDB_OK;
}

int ezdb_entries_section_build_add(EzdbEntrySectionBuild* build,
                                   const EzdbEntryRecord* record,
                                   uint32_t final_archive_id)
{
    if (!build || !build->writer_ready) return EZDB_ERR_ARG;
    return ezdb_entries_section_writer_add(&build->writer, record, final_archive_id);
}

int ezdb_entries_section_build_finish(EzdbEntrySectionBuild* build)
{
    if (!build || !build->writer_ready) return EZDB_ERR_ARG;
    int rc = ezdb_entries_section_writer_finish(&build->writer);
    if (build->core_fp && fclose(build->core_fp) != 0 && rc == EZDB_OK) rc = EZDB_ERR_IO;
    build->core_fp = NULL;
    if (build->detail_fp && fclose(build->detail_fp) != 0 && rc == EZDB_OK) rc = EZDB_ERR_IO;
    build->detail_fp = NULL;
    if (build->raw_fp && fclose(build->raw_fp) != 0 && rc == EZDB_OK) rc = EZDB_ERR_IO;
    build->raw_fp = NULL;
    return rc;
}

int ezdb_entries_section_build_write_core(EzdbEntrySectionBuild* build,
                                          FILE* out,
                                          EzdbHeader* header,
                                          uint32_t entry_count)
{
    if (!build || !out || !header) return EZDB_ERR_ARG;
    uint64_t written = 0;
    header->entry_records_offset = (uint64_t)_ftelli64(out);
    header->entry_records_raw_size = EZDB_ENTRY_CORE_RECORD_SIZE * (uint64_t)entry_count;
    header->entry_records_flags = 0;
    int rc = entries_copy_file_payload(out, build->core_path, &written);
    header->entry_records_size = written;
    return rc;
}

int ezdb_entries_section_build_write_detail(EzdbEntrySectionBuild* build, FILE* out, EzdbHeader* header)
{
    if (!build || !out || !header) return EZDB_ERR_ARG;
    uint64_t detail_written = 0;
    header->entry_detail_offset = (uint64_t)_ftelli64(out);
    int rc = entries_copy_file_payload(out, build->detail_path, &detail_written);
    header->entry_detail_size = detail_written;
    header->entry_detail_index_offset = (uint64_t)_ftelli64(out);
    header->entry_detail_page_count = build->writer.detail_writer.page_count;
    header->entry_page_size = EZDB_ENTRY_PAGE_SIZE;
    if (rc == EZDB_OK && build->writer.detail_writer.page_count &&
        fwrite(build->writer.detail_writer.pages,
               sizeof(EzdbDiskPage),
               build->writer.detail_writer.page_count,
               out) != build->writer.detail_writer.page_count) {
        rc = EZDB_ERR_IO;
    }
    return rc;
}

int ezdb_entries_section_build_write_raw(EzdbEntrySectionBuild* build, FILE* out, EzdbHeader* header)
{
    if (!build || !out || !header) return EZDB_ERR_ARG;
    uint64_t raw_written = 0;
    header->raw_blob_offset = (uint64_t)_ftelli64(out);
    header->raw_blob_raw_size = build->writer.raw_writer.raw_size;
    int rc = entries_copy_file_payload(out, build->raw_path, &raw_written);
    header->raw_blob_size = raw_written;
    header->raw_blob_index_offset = (uint64_t)_ftelli64(out);
    header->raw_blob_page_count = build->writer.raw_writer.page_count;
    header->raw_blob_page_size = EZDB_RAW_BLOB_PAGE_SIZE;
    if (rc == EZDB_OK && build->writer.raw_writer.page_count &&
        fwrite(build->writer.raw_writer.pages,
               sizeof(EzdbDiskPage),
               build->writer.raw_writer.page_count,
               out) != build->writer.raw_writer.page_count) {
        rc = EZDB_ERR_IO;
    }
    return rc;
}

void ezdb_entries_section_build_free(EzdbEntrySectionBuild* build)
{
    if (!build) return;
    if (build->core_fp) fclose(build->core_fp);
    if (build->detail_fp) fclose(build->detail_fp);
    if (build->raw_fp) fclose(build->raw_fp);
    if (build->writer_ready) ezdb_entries_section_writer_free(&build->writer);
    remove(build->core_path);
    remove(build->detail_path);
    remove(build->raw_path);
    memset(build, 0, sizeof(*build));
}

int ezdb_entries_collect_sections(EzdbEntryCollectResult* result,
                                  EzdbEntrySource* source,
                                  uint32_t entry_count,
                                  uint32_t original_archive_count,
                                  const uint32_t* original_to_final,
                                  uint64_t final_archive_count,
                                  const char* temp_dir,
                                  int track_archive_counts,
                                  EzdbEntryCollectPathCallback path_callback,
                                  void* path_callback_user_data)
{
    if (!result || (!source && entry_count) || (!original_to_final && entry_count) || !temp_dir) return EZDB_ERR_ARG;
    memset(result, 0, sizeof(*result));
    result->last_entry_archive_id = UINT32_MAX;
    int rc = ezdb_entries_section_build_begin(&result->sections, temp_dir);
    if (rc != EZDB_OK) return rc;
    result->sections_ready = 1;
    if (track_archive_counts && original_archive_count) {
        result->archive_entry_counts = (uint32_t*)calloc((size_t)original_archive_count, sizeof(uint32_t));
        result->archive_entry_bases = (uint32_t*)malloc(sizeof(uint32_t) * (size_t)original_archive_count);
        if (!result->archive_entry_counts || !result->archive_entry_bases) {
            ezdb_entries_collect_result_free(result);
            return EZDB_ERR_MEMORY;
        }
        for (uint32_t i = 0; i < original_archive_count; ++i) result->archive_entry_bases[i] = UINT32_MAX;
        result->parallel_count_possible = 1;
    }
    if (source && source->reset) {
        rc = source->reset(source->user_data);
        if (rc != EZDB_OK) {
            ezdb_entries_collect_result_free(result);
            return rc;
        }
    }
    for (uint32_t i = 0; rc == EZDB_OK && i < entry_count; ++i) {
        EzdbEntryRecord record;
        memset(&record, 0, sizeof(record));
        rc = source->next(source->user_data, &record);
        if (rc != EZDB_OK) break;
        if (!record.entry_path || record.archive_id >= original_archive_count ||
            original_to_final[record.archive_id] == UINT32_MAX) {
            rc = EZDB_ERR_ARG;
            break;
        }
        uint32_t final_archive_id = original_to_final[record.archive_id];
        if (final_archive_id >= final_archive_count) {
            rc = EZDB_ERR_ARG;
            break;
        }
        if (result->parallel_count_possible) {
            if (result->last_entry_archive_id != UINT32_MAX && record.archive_id < result->last_entry_archive_id) {
                rc = EZDB_ERR_ARG;
                break;
            }
            if (result->archive_entry_bases[record.archive_id] == UINT32_MAX) {
                result->archive_entry_bases[record.archive_id] = i;
            } else if (i != result->archive_entry_bases[record.archive_id] +
                            result->archive_entry_counts[record.archive_id]) {
                rc = EZDB_ERR_ARG;
                break;
            }
            result->archive_entry_counts[record.archive_id] += 1u;
            result->last_entry_archive_id = record.archive_id;
        }
        rc = ezdb_entries_section_build_add(&result->sections, &record, final_archive_id);
        if (rc != EZDB_OK) break;
        if (path_callback) {
            rc = path_callback(path_callback_user_data, record.entry_path, i);
            if (rc != EZDB_OK) break;
        }
    }
    if (rc == EZDB_OK) {
        rc = ezdb_entries_section_build_finish(&result->sections);
    }
    if (rc != EZDB_OK) ezdb_entries_collect_result_free(result);
    return rc;
}

int ezdb_entries_write_collected_sections(EzdbEntryCollectResult* result,
                                          FILE* out,
                                          EzdbHeader* header,
                                          uint32_t entry_count,
                                          EzdbEntryFinalizeStats* stats)
{
    if (!result || !out || !header) return EZDB_ERR_ARG;
    if (stats) memset(stats, 0, sizeof(*stats));

    double stage_start_ms = ezdb_now_ms();
    int rc = ezdb_entries_section_build_write_core(&result->sections, out, header, entry_count);
    if (stats) stats->write_core_ms = ezdb_now_ms() - stage_start_ms;

    if (rc == EZDB_OK) {
        stage_start_ms = ezdb_now_ms();
        rc = ezdb_entries_section_build_write_detail(&result->sections, out, header);
        if (stats) stats->write_detail_ms = ezdb_now_ms() - stage_start_ms;
    }
    if (rc == EZDB_OK) {
        stage_start_ms = ezdb_now_ms();
        rc = ezdb_entries_section_build_write_raw(&result->sections, out, header);
        if (stats) stats->write_raw_ms = ezdb_now_ms() - stage_start_ms;
    }
    if (rc == EZDB_OK) {
        stage_start_ms = ezdb_now_ms();
        header->entry_count = entry_count;
        header->active_entry_count = entry_count;
        header->base_entry_count = entry_count;
        header->reserved_offset = (uint64_t)_ftelli64(out);
        header->reserved_size = 0;
        header->delta_offset = header->reserved_offset;
        header->delta_size = 0;
        if (stats) stats->finalize_ms = ezdb_now_ms() - stage_start_ms;
    }
    return rc;
}

void ezdb_entries_collect_result_free(EzdbEntryCollectResult* result)
{
    if (!result) return;
    if (result->sections_ready) ezdb_entries_section_build_free(&result->sections);
    free(result->archive_entry_counts);
    free(result->archive_entry_bases);
    memset(result, 0, sizeof(*result));
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
    if (_fseeki64(fp, (__int64)offset, SEEK_SET) != 0) return EZDB_ERR_IO;
    if (len && fread(out, 1, len, fp) != len) return EZDB_ERR_IO;
    return EZDB_OK;
}

int ezdb_entries_load_detail(const EzdbEntryDetailStore* store, uint32_t id, EzdbDiskEntry* out)
{
    if (!store || !out || id >= store->entry_count || !store->archive_ids ||
        !store->path_offsets || !store->path_lens) {
        return EZDB_ERR_ARG;
    }
    if (ezdb_bitset_get(store->delta_bits, id)) {
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
    __int64 saved_pos = _ftelli64(store->fp);
    int rc = ezdb_bitset_get(store->delta_bits, id)
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
    if (saved_pos >= 0 && _fseeki64(store->fp, saved_pos, SEEK_SET) != 0) rc = EZDB_ERR_IO;
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
    int rc = ezdb_bitset_get(store->delta_bits, id)
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
        !ezdb_bitset_get(source->active_entry_bits, entry_id)) {
        return 0;
    }
    if (!source->detail_store.archive_ids) return 0;
    uint32_t archive_id = source->detail_store.archive_ids[entry_id];
    return archive_id < source->file_count && ezdb_bitset_get(source->active_archive_bits, archive_id);
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
