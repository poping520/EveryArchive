#include "ezdb_build.h"

#include "ezdb_internal.h"
#include "ezdb_postings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct EzdbPublicEntryStreamRange {
    EzdbEntryStream stream;
} EzdbPublicEntryStreamRange;

static double build_now_ms(void)
{
    return (double)clock() * 1000.0 / (double)CLOCKS_PER_SEC;
}

static int build_ensure_capacity(void** data, size_t elem_size, uint32_t* capacity, uint32_t needed)
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

static uint32_t build_fnv1a_bytes(const char* text, size_t len)
{
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < len; ++i) {
        hash ^= (unsigned char)text[i];
        hash *= 16777619u;
    }
    return hash;
}

static int build_append_string(char** data, uint32_t* size, uint32_t* cap, const char* text, uint32_t len, uint32_t* out_offset)
{
    if (build_ensure_capacity((void**)data, 1, cap, *size + len + 1u) != EZDB_OK) return EZDB_ERR_MEMORY;
    *out_offset = *size;
    memcpy(*data + *size, text, len);
    *size += len;
    (*data)[(*size)++] = '\0';
    return EZDB_OK;
}

static int build_init_u32_buckets(uint32_t** buckets, uint32_t count)
{
    *buckets = (uint32_t*)malloc(sizeof(uint32_t) * count);
    if (!*buckets) return EZDB_ERR_MEMORY;
    for (uint32_t i = 0; i < count; ++i) (*buckets)[i] = UINT32_MAX;
    return EZDB_OK;
}

static int build_find_or_add_string(EzdbArchiveBuildTree* tree, const char* text, uint32_t len, uint32_t* out_offset)
{
    if (!tree->string_buckets) {
        tree->string_bucket_count = 65536u;
        if (build_init_u32_buckets(&tree->string_buckets, tree->string_bucket_count) != EZDB_OK) return EZDB_ERR_MEMORY;
    }
    uint32_t hash = build_fnv1a_bytes(text, len);
    uint32_t bucket = hash & (tree->string_bucket_count - 1u);
    for (uint32_t i = tree->string_buckets[bucket]; i != UINT32_MAX; i = tree->string_entries[i].next) {
        if (tree->string_entries[i].len == len &&
            memcmp(tree->string_pool + tree->string_entries[i].offset, text, len) == 0) {
            *out_offset = tree->string_entries[i].offset;
            return EZDB_OK;
        }
    }

    uint32_t offset = 0;
    if (build_append_string(&tree->string_pool, &tree->string_size, &tree->string_cap, text, len, &offset) != EZDB_OK) {
        return EZDB_ERR_MEMORY;
    }
    if (build_ensure_capacity((void**)&tree->string_entries,
                              sizeof(EzdbBuildStringHashEntry),
                              &tree->string_entry_cap,
                              tree->string_entry_count + 1) != EZDB_OK) {
        return EZDB_ERR_MEMORY;
    }
    EzdbBuildStringHashEntry* e = &tree->string_entries[tree->string_entry_count];
    e->offset = offset;
    e->len = len;
    e->next = tree->string_buckets[bucket];
    tree->string_buckets[bucket] = tree->string_entry_count;
    tree->string_entry_count += 1;
    *out_offset = offset;
    return EZDB_OK;
}

static uint32_t build_find_or_add_dir(EzdbArchiveBuildTree* tree,
                                      uint32_t parent,
                                      const char* name,
                                      uint32_t name_len)
{
    if (!tree->dir_buckets) {
        tree->dir_bucket_count = 262144u;
        if (build_init_u32_buckets(&tree->dir_buckets, tree->dir_bucket_count) != EZDB_OK) return UINT32_MAX;
    }
    uint32_t hash = build_fnv1a_bytes(name, name_len) ^ (parent * 16777619u);
    uint32_t bucket = hash & (tree->dir_bucket_count - 1u);
    for (uint32_t i = tree->dir_buckets[bucket]; i != UINT32_MAX; i = tree->dir_hash_entries[i].next) {
        EzdbBuildDirHashEntry* he = &tree->dir_hash_entries[i];
        if (he->parent == parent && he->name_len == name_len &&
            memcmp(tree->string_pool + he->name_offset, name, name_len) == 0) {
            return he->dir_id;
        }
    }

    if (build_ensure_capacity((void**)&tree->dirs, sizeof(EzdbBuildDir), &tree->dir_cap, tree->dir_count + 1) != EZDB_OK) {
        return UINT32_MAX;
    }
    uint32_t name_offset = 0;
    if (build_find_or_add_string(tree, name, name_len, &name_offset) != EZDB_OK) return UINT32_MAX;

    uint32_t id = tree->dir_count;
    EzdbBuildDir* dir = &tree->dirs[id];
    memset(dir, 0, sizeof(*dir));
    dir->name_offset = name_offset;
    dir->name_len = name_len;
    dir->parent = parent;
    dir->first_child = UINT32_MAX;
    dir->next_sibling = UINT32_MAX;
    dir->first_file = UINT32_MAX;
    dir->old_first_file = UINT32_MAX;
    if (id != parent && parent != UINT32_MAX) {
        dir->next_sibling = tree->dirs[parent].first_child;
        tree->dirs[parent].first_child = id;
    }
    tree->dir_count += 1;

    if (build_ensure_capacity((void**)&tree->dir_hash_entries,
                              sizeof(EzdbBuildDirHashEntry),
                              &tree->dir_hash_cap,
                              tree->dir_hash_count + 1) != EZDB_OK) {
        return UINT32_MAX;
    }
    EzdbBuildDirHashEntry* he = &tree->dir_hash_entries[tree->dir_hash_count];
    he->parent = parent;
    he->name_offset = name_offset;
    he->name_len = name_len;
    he->dir_id = id;
    he->next = tree->dir_buckets[bucket];
    tree->dir_buckets[bucket] = tree->dir_hash_count;
    tree->dir_hash_count += 1;
    return id;
}

static uint32_t build_get_or_create_path_dir(EzdbArchiveBuildTree* tree, const char* path, uint32_t path_len)
{
    uint32_t parent = 0;
    uint32_t start = 0;
    for (uint32_t i = 0; i <= path_len; ++i) {
        if (i == path_len || path[i] == '\\' || path[i] == '/') {
            if (i > start) {
                parent = build_find_or_add_dir(tree, parent, path + start, i - start);
                if (parent == UINT32_MAX) return UINT32_MAX;
            }
            start = i + 1;
        }
    }
    return parent;
}

static int build_append_file(EzdbArchiveBuildTree* tree,
                             uint32_t dir_id,
                             const char* name,
                             uint32_t name_len,
                             uint32_t original_id,
                             uint64_t size,
                             uint64_t modified_time,
                             char drive_letter,
                             uint64_t file_ref_number,
                             int64_t usn)
{
    if (build_ensure_capacity((void**)&tree->old_files, sizeof(EzdbBuildFile), &tree->file_cap, tree->file_count + 1) != EZDB_OK) {
        return EZDB_ERR_MEMORY;
    }
    uint32_t name_offset = 0;
    if (build_find_or_add_string(tree, name, name_len, &name_offset) != EZDB_OK) return EZDB_ERR_MEMORY;
    uint32_t id = tree->file_count;
    EzdbBuildFile* f = &tree->old_files[id];
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
    f->next_in_dir = tree->dirs[dir_id].old_first_file;
    tree->dirs[dir_id].old_first_file = id;
    tree->dirs[dir_id].old_file_count += 1;
    tree->file_count += 1;
    return EZDB_OK;
}

static int build_append_varuint(unsigned char** data, uint32_t* size, uint32_t* cap, uint32_t value)
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

static int build_append_varuint64(unsigned char** data, uint32_t* size, uint32_t* cap, uint64_t value)
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

static uint32_t build_dfs_assign(EzdbBuildDir* dirs,
                                 EzdbBuildFile* old_files,
                                 EzdbBuildFile* new_files,
                                 uint32_t dir_id,
                                 uint32_t next_file,
                                 uint32_t* original_to_final)
{
    EzdbBuildDir* dir = &dirs[dir_id];
    dir->first_file_id = next_file;
    for (uint32_t old = dir->old_first_file; old != UINT32_MAX; old = old_files[old].next_in_dir) {
        new_files[next_file] = old_files[old];
        new_files[next_file].parent_dir = dir_id;
        if (original_to_final) original_to_final[old_files[old].original_id] = next_file;
        ++next_file;
    }
    for (uint32_t child = dir->first_child; child != UINT32_MAX; child = dirs[child].next_sibling) {
        next_file = build_dfs_assign(dirs, old_files, new_files, child, next_file, original_to_final);
    }
    dir->file_count = next_file - dir->first_file_id;
    return next_file;
}

int ezdb_build_archive_tree_init(EzdbArchiveBuildTree* tree)
{
    if (!tree) return EZDB_ERR_ARG;
    memset(tree, 0, sizeof(*tree));
    if (build_ensure_capacity((void**)&tree->dirs, sizeof(EzdbBuildDir), &tree->dir_cap, 1) != EZDB_OK) {
        return EZDB_ERR_MEMORY;
    }
    memset(&tree->dirs[0], 0, sizeof(EzdbBuildDir));
    tree->dirs[0].parent = 0;
    tree->dirs[0].first_child = UINT32_MAX;
    tree->dirs[0].next_sibling = UINT32_MAX;
    tree->dirs[0].first_file = UINT32_MAX;
    tree->dirs[0].old_first_file = UINT32_MAX;
    tree->dir_count = 1;
    return EZDB_OK;
}

int ezdb_build_archive_tree_add_archives(EzdbArchiveBuildTree* tree,
                                         const EzdbArchiveRecord* archives,
                                         uint32_t archive_count)
{
    if (!tree || (!archives && archive_count)) return EZDB_ERR_ARG;
    for (uint32_t i = 0; i < archive_count; ++i) {
        const EzdbArchiveRecord* archive = &archives[i];
        const char* path = archive->file_path;
        if (!path || !*path) return EZDB_ERR_ARG;
        char* slash = strrchr(path, '\\');
        char* fslash = strrchr(path, '/');
        if (!slash || (fslash && fslash > slash)) slash = fslash;
        const char* name = slash ? slash + 1 : path;
        uint32_t name_len = (uint32_t)strlen(name);
        uint32_t dir_id = 0;
        if (slash) {
            dir_id = build_get_or_create_path_dir(tree, path, (uint32_t)(slash - path));
            if (dir_id == UINT32_MAX) return EZDB_ERR_MEMORY;
        }
        int rc = build_append_file(tree,
                                   dir_id,
                                   name,
                                   name_len,
                                   i,
                                   archive->file_size,
                                   archive->modified_time,
                                   archive->drive_letter,
                                   archive->file_ref_number,
                                   archive->usn);
        if (rc != EZDB_OK) return rc;
    }
    return EZDB_OK;
}

int ezdb_build_archive_tree_assign(EzdbArchiveBuildTree* tree, uint32_t* original_to_final)
{
    if (!tree) return EZDB_ERR_ARG;
    tree->files = (EzdbBuildFile*)malloc(sizeof(EzdbBuildFile) * (size_t)tree->file_count);
    if (!tree->files && tree->file_count) return EZDB_ERR_MEMORY;
    uint32_t assigned = build_dfs_assign(tree->dirs, tree->old_files, tree->files, 0, 0, original_to_final);
    if (assigned != tree->file_count) return EZDB_ERR_FORMAT;
    free(tree->old_files);
    tree->old_files = NULL;
    return EZDB_OK;
}

void ezdb_build_archive_tree_free(EzdbArchiveBuildTree* tree)
{
    if (!tree) return;
    free(tree->dirs);
    free(tree->old_files);
    free(tree->files);
    free(tree->dir_hash_entries);
    free(tree->dir_buckets);
    free(tree->string_pool);
    free(tree->string_entries);
    free(tree->string_buckets);
    memset(tree, 0, sizeof(*tree));
}

int ezdb_build_encode_file_records_compact(const EzdbBuildFile* files,
                                           uint32_t file_count,
                                           unsigned char** out_data,
                                           uint64_t* out_size)
{
    if ((!files && file_count) || !out_data || !out_size) return EZDB_ERR_ARG;
    unsigned char* data = NULL;
    uint32_t size = 0, cap = 0;
    for (uint32_t i = 0; i < file_count; ++i) {
        int rc = build_append_varuint(&data, &size, &cap, files[i].parent_dir);
        if (rc == EZDB_OK) rc = build_append_varuint(&data, &size, &cap, files[i].name_offset);
        if (rc == EZDB_OK) rc = build_append_varuint(&data, &size, &cap, files[i].name_len);
        if (rc == EZDB_OK) rc = build_append_varuint64(&data, &size, &cap, files[i].size);
        if (rc == EZDB_OK) rc = build_append_varuint64(&data, &size, &cap, files[i].modified_time);
        if (rc != EZDB_OK) {
            free(data);
            return rc;
        }
    }
    *out_data = data;
    *out_size = size;
    return EZDB_OK;
}

int ezdb_build_write_archive_postings(FILE* out,
                                      const EzdbArchiveBuildTree* tree,
                                      EzdbBuildArchivePostingsResult* result)
{
    if (!out || !tree || !result) return EZDB_ERR_ARG;
    memset(result, 0, sizeof(*result));
    PostingBuilder file_builder;
    PostingBuilder dir_builder;
    int file_builder_ready = 0;
    int dir_builder_ready = 0;
    memset(&file_builder, 0, sizeof(file_builder));
    memset(&dir_builder, 0, sizeof(dir_builder));

    double stage_start_ms = build_now_ms();
    int rc = ezdb_postings_builder_init(&file_builder, 262144u);
    if (rc == EZDB_OK) file_builder_ready = 1;
    if (rc == EZDB_OK) {
        for (uint32_t i = 0; i < tree->file_count; ++i) {
            rc = ezdb_postings_count_text_grams(&file_builder, tree->string_pool + tree->files[i].name_offset, i);
            if (rc != EZDB_OK) break;
        }
    }
    if (rc == EZDB_OK) rc = ezdb_postings_builder_prepare_fill(&file_builder);
    if (rc == EZDB_OK) {
        for (uint32_t i = 0; i < tree->file_count; ++i) {
            rc = ezdb_postings_fill_text_grams(&file_builder, tree->string_pool + tree->files[i].name_offset, i);
            if (rc != EZDB_OK) break;
        }
    }
    if (rc == EZDB_OK) {
        rc = ezdb_postings_write(out,
                                 &file_builder,
                                 tree->file_count,
                                 &result->file_index,
                                 &result->file_index_count,
                                 &result->file_postings_size,
                                 NULL);
    }
    if (rc == EZDB_OK) result->file_index_ms = build_now_ms() - stage_start_ms;
    if (file_builder_ready) {
        ezdb_postings_builder_free(&file_builder);
        file_builder_ready = 0;
    }

    if (rc == EZDB_OK) {
        stage_start_ms = build_now_ms();
        rc = ezdb_postings_builder_init(&dir_builder, 131072u);
        if (rc == EZDB_OK) dir_builder_ready = 1;
    }
    if (rc == EZDB_OK) {
        for (uint32_t i = 1; i < tree->dir_count; ++i) {
            rc = ezdb_postings_count_text_grams(&dir_builder, tree->string_pool + tree->dirs[i].name_offset, i);
            if (rc != EZDB_OK) break;
        }
    }
    if (rc == EZDB_OK) rc = ezdb_postings_builder_prepare_fill(&dir_builder);
    if (rc == EZDB_OK) {
        for (uint32_t i = 1; i < tree->dir_count; ++i) {
            rc = ezdb_postings_fill_text_grams(&dir_builder, tree->string_pool + tree->dirs[i].name_offset, i);
            if (rc != EZDB_OK) break;
        }
    }
    if (rc == EZDB_OK) {
        rc = ezdb_postings_write(out,
                                 &dir_builder,
                                 tree->dir_count,
                                 &result->dir_index,
                                 &result->dir_index_count,
                                 &result->dir_postings_size,
                                 NULL);
    }
    if (rc == EZDB_OK) result->dir_index_ms = build_now_ms() - stage_start_ms;
    if (dir_builder_ready) {
        ezdb_postings_builder_free(&dir_builder);
        dir_builder_ready = 0;
    }
    if (rc != EZDB_OK) ezdb_build_archive_postings_result_free(result);
    return rc;
}

void ezdb_build_archive_postings_result_free(EzdbBuildArchivePostingsResult* result)
{
    if (!result) return;
    free(result->file_index);
    free(result->dir_index);
    memset(result, 0, sizeof(*result));
}

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
