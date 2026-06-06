#pragma once

#include "ezdb.h"
#include "ezdb_internal.h"

#include <stdint.h>

#define EZDB_ENTRY_PAGE_SIZE 4096u
#define EZDB_RAW_BLOB_PAGE_SIZE (256u * 1024u)
#define EZDB_ENTRY_DETAIL_CACHE_PAGES 8u
#define EZDB_RAW_BLOB_CACHE_PAGES 64u
#define EZDB_ENTRY_CORE_RECORD_SIZE 12u

typedef struct EzdbEntrySource {
    void* user_data;
    int (*reset)(void* user_data);
    int (*reset_range)(void* user_data, uint32_t archive_begin, uint32_t archive_end);
    int (*next)(void* user_data, EzdbEntryRecord* out_record);
    int (*open_range)(void* user_data, uint32_t archive_begin, uint32_t archive_end, struct EzdbEntrySource* out_source);
    void (*close_range)(struct EzdbEntrySource* source);
} EzdbEntrySource;

void ezdb_entries_encode_core(const EzdbDiskEntry* entry, unsigned char out[EZDB_ENTRY_CORE_RECORD_SIZE]);
void ezdb_entries_decode_core(const unsigned char raw[EZDB_ENTRY_CORE_RECORD_SIZE], EzdbDiskEntry* out);
