#include "ezdb_build.h"

#include "ezdb_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static int public_entry_stream_open_range(void* user_data,
                                          uint32_t archive_begin,
                                          uint32_t archive_end,
                                          EzdbEntrySource* out_source)
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
        archive_id_map = (uint32_t*)malloc(sizeof(uint32_t) * (size_t)archive_count);
        if (!archive_id_map) {
            rc = EZDB_ERR_MEMORY;
        } else {
            for (uint32_t i = 0; i < archive_count; ++i) archive_id_map[i] = UINT32_MAX;
        }
    }
    EzdbBuildOptionsResolved options;
    if (rc == EZDB_OK) rc = ezdb_build_resolve_options(output_ezdb, NULL, &options);
    if (rc == EZDB_OK) {
        rc = ezdb_build_write_archive_base_core(archives,
                                                archive_count,
                                                entries,
                                                entry_count,
                                                output_ezdb,
                                                archive_id_map,
                                                NULL,
                                                &options);
    }
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
    rc = ezdb_build_resolve_options(output_ezdb, build_options, &options);
    if (archive_count) {
        archive_id_map = (uint32_t*)malloc(sizeof(uint32_t) * (size_t)archive_count);
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
    if (rc == EZDB_OK) {
        rc = ezdb_build_write_archive_base_core(archives,
                                                archive_count,
                                                NULL,
                                                entry_count,
                                                output_ezdb,
                                                archive_id_map,
                                                entry_count ? &source : NULL,
                                                &options);
    }
    free(archive_id_map);
    if (rc != EZDB_OK) remove(output_ezdb);
    return rc;
}
