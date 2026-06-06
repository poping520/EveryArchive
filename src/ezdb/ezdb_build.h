#pragma once

#include "ezdb_entries.h"

#include <stdint.h>

typedef struct EzdbBuildOptionsResolved {
    char temp_dir[1024];
    uint32_t memory_limit_mb;
    uint32_t flags;
    uint32_t log_level;
    uint32_t index_threads;
    uint32_t zip_threads;
} EzdbBuildOptionsResolved;

typedef struct EzdbBuildDir {
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
} EzdbBuildDir;

typedef struct EzdbBuildFile {
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
} EzdbBuildFile;

typedef struct EzdbBuildDirHashEntry {
    uint32_t parent;
    uint32_t name_offset;
    uint32_t name_len;
    uint32_t dir_id;
    uint32_t next;
} EzdbBuildDirHashEntry;

typedef struct EzdbBuildStringHashEntry {
    uint32_t offset;
    uint32_t len;
    uint32_t next;
} EzdbBuildStringHashEntry;

typedef struct EzdbArchiveBuildTree {
    EzdbBuildDir* dirs;
    EzdbBuildFile* old_files;
    EzdbBuildFile* files;
    uint32_t dir_count;
    uint32_t dir_cap;
    uint32_t file_count;
    uint32_t file_cap;
    EzdbBuildDirHashEntry* dir_hash_entries;
    uint32_t dir_hash_count;
    uint32_t dir_hash_cap;
    uint32_t dir_bucket_count;
    uint32_t* dir_buckets;
    char* string_pool;
    uint32_t string_size;
    uint32_t string_cap;
    EzdbBuildStringHashEntry* string_entries;
    uint32_t string_entry_count;
    uint32_t string_entry_cap;
    uint32_t string_bucket_count;
    uint32_t* string_buckets;
} EzdbArchiveBuildTree;

int ezdb_build_resolve_options(const char* output_ezdb,
                               const EzdbBuildOptions* options,
                               EzdbBuildOptionsResolved* out);
int ezdb_build_archive_tree_init(EzdbArchiveBuildTree* tree);
int ezdb_build_archive_tree_add_archives(EzdbArchiveBuildTree* tree,
                                         const EzdbArchiveRecord* archives,
                                         uint32_t archive_count);
int ezdb_build_archive_tree_assign(EzdbArchiveBuildTree* tree, uint32_t* original_to_final);
void ezdb_build_archive_tree_free(EzdbArchiveBuildTree* tree);
int ezdb_build_encode_file_records_compact(const EzdbBuildFile* files,
                                           uint32_t file_count,
                                           unsigned char** out_data,
                                           uint64_t* out_size);
int ezdb_build_write_archive_base_core(const EzdbArchiveRecord* archives,
                                       uint32_t archive_count,
                                       const EzdbEntryRecord* entries,
                                       uint32_t entry_count,
                                       const char* output_ezdb,
                                       uint32_t* original_to_final,
                                       EzdbEntrySource* entry_source,
                                       const EzdbBuildOptionsResolved* options);
