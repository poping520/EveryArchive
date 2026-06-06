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

int ezdb_build_resolve_options(const char* output_ezdb,
                               const EzdbBuildOptions* options,
                               EzdbBuildOptionsResolved* out);
int ezdb_build_write_archive_base_core(const EzdbArchiveRecord* archives,
                                       uint32_t archive_count,
                                       const EzdbEntryRecord* entries,
                                       uint32_t entry_count,
                                       const char* output_ezdb,
                                       uint32_t* original_to_final,
                                       EzdbEntrySource* entry_source,
                                       const EzdbBuildOptionsResolved* options);
