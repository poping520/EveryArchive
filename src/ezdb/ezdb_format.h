#pragma once

#include <stdint.h>
#include <stdio.h>

#define EZDB_MAGIC "EZDB0012"
#define EZDB_VERSION 12u
#define EZDB_V13_MAGIC "EZDB0013"
#define EZDB_V13_VERSION 13u

#define EZDB_FORMAT_OK 0
#define EZDB_FORMAT_ERR_ARG -1
#define EZDB_FORMAT_ERR_IO -2
#define EZDB_FORMAT_ERR_FORMAT -3
#define EZDB_FORMAT_ERR_MEMORY -4

#define EZDB_SECTION_ARCHIVE_RECORDS 1u
#define EZDB_SECTION_DIR_RECORDS 2u
#define EZDB_SECTION_STRING_POOL 3u
#define EZDB_SECTION_ARCHIVE_META 4u
#define EZDB_SECTION_ENTRY_CORE 5u
#define EZDB_SECTION_ENTRY_DETAIL_PAGES 6u
#define EZDB_SECTION_ENTRY_RAW_PAGES 7u
#define EZDB_SECTION_FILE_INDEX 8u
#define EZDB_SECTION_DIR_INDEX 9u
#define EZDB_SECTION_ENTRY_INDEX 10u
#define EZDB_SECTION_POSTINGS 11u
#define EZDB_SECTION_DELTA_LOG 12u
#define EZDB_SECTION_METADATA 13u

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

typedef struct EzdbV13Header {
    char magic[8];
    uint32_t version;
    uint32_t header_size;
    uint32_t flags;
    uint32_t section_count;
    uint64_t section_table_offset;
    uint64_t archive_count;
    uint64_t active_archive_count;
    uint64_t dir_count;
    uint64_t entry_count;
    uint64_t active_entry_count;
    uint64_t base_archive_count;
    uint64_t base_entry_count;
    uint64_t metadata_offset;
    uint64_t metadata_size;
    uint64_t checksum;
    uint64_t reserved[4];
} EzdbV13Header;

typedef struct EzdbSectionDesc {
    uint32_t section_id;
    uint32_t flags;
    uint64_t offset;
    uint64_t encoded_size;
    uint64_t raw_size;
    uint64_t aux_offset;
    uint64_t aux_size;
    uint32_t page_size;
    uint32_t aux_count;
} EzdbSectionDesc;

int ezdb_format_header_is_current(const EzdbHeader* header);
int ezdb_format_v13_header_is_current(const EzdbV13Header* header);
int ezdb_format_section_id_is_known(uint32_t section_id);
int ezdb_format_validate_section_table(const EzdbSectionDesc* sections, uint32_t section_count, uint64_t file_size);
const EzdbSectionDesc* ezdb_format_find_section(const EzdbSectionDesc* sections, uint32_t section_count, uint32_t section_id);
int ezdb_format_write_section_table(FILE* fp,
                                    const EzdbSectionDesc* sections,
                                    uint32_t section_count,
                                    uint64_t* out_offset,
                                    uint64_t* out_size);
int ezdb_format_read_section_table(FILE* fp,
                                   const EzdbV13Header* header,
                                   uint64_t file_size,
                                   EzdbSectionDesc** out_sections);
