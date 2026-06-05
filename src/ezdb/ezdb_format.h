#pragma once

#include <stdint.h>

#define EZDB_MAGIC "EZDB0012"
#define EZDB_VERSION 12u
#define EZDB_DELTA_MAGIC 0x31445a45u
#define EZDB_DELTA_INSERT 1u
#define EZDB_DELTA_UPDATE 2u
#define EZDB_DELTA_DELETE 3u
#define EZDB_DELTA_BATCH_BEGIN 4u
#define EZDB_DELTA_BATCH_COMMIT 5u
#define EZDB_DELTA_ENTRY_DELETE_ARCHIVE 6u
#define EZDB_DELTA_ENTRY_APPEND 7u

typedef struct EzdbHeader {
    char magic[8];
    uint32_t version;
    uint32_t header_size;
    uint64_t file_count;
    uint64_t active_count;
    uint64_t dir_count;
    uint64_t file_records_offset;
    uint64_t file_records_size;
    uint64_t dir_records_offset;
    uint64_t dir_records_size;
    uint64_t strings_offset;
    uint64_t strings_size;
    uint64_t file_index_offset;
    uint64_t file_index_count;
    uint64_t dir_index_offset;
    uint64_t dir_index_count;
    uint64_t postings_offset;
    uint64_t postings_size;
    uint64_t reserved_offset;
    uint64_t reserved_size;
    uint64_t file_records_raw_size;
    uint64_t dir_records_raw_size;
    uint64_t strings_raw_size;
    uint32_t file_records_flags;
    uint32_t dir_records_flags;
    uint32_t strings_flags;
    uint32_t reserved_flags;
    uint64_t base_file_count;
    uint64_t delta_offset;
    uint64_t delta_size;
    uint64_t archive_meta_offset;
    uint64_t archive_meta_size;
    uint64_t archive_meta_raw_size;
    uint32_t archive_meta_flags;
    uint32_t archive_meta_reserved;
    uint64_t entry_records_offset;
    uint64_t entry_records_size;
    uint64_t entry_records_raw_size;
    uint32_t entry_records_flags;
    uint32_t entry_records_reserved;
    uint64_t raw_blob_offset;
    uint64_t raw_blob_size;
    uint64_t raw_blob_raw_size;
    uint32_t raw_blob_flags;
    uint32_t raw_blob_reserved;
    uint64_t entry_count;
    uint64_t active_entry_count;
    uint64_t entry_index_offset;
    uint64_t entry_index_count;
    uint64_t entry_postings_size;
    uint64_t entry_detail_offset;
    uint64_t entry_detail_size;
    uint64_t entry_detail_index_offset;
    uint64_t entry_detail_page_count;
    uint64_t raw_blob_index_offset;
    uint64_t raw_blob_page_count;
    uint32_t entry_page_size;
    uint32_t raw_blob_page_size;
    uint64_t base_entry_count;
} EzdbHeader;

int ezdb_format_header_is_current(const EzdbHeader* header);
