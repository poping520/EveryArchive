#pragma once

#include <stdint.h>

#define EZDB_POSTING_COMPRESS_MIN_SIZE 256u
#define EZDB_POSTING_COMPRESS_MIN_SAVING 16u
#define EZDB_POSTING_COMPRESSION_LEVEL 1
#define EZDB_SECTION_COMPRESSED 1u
#define EZDB_SECTION_COMPRESS_MIN_SIZE 4096u
#define EZDB_SECTION_COMPRESS_MIN_SAVING 256u
#define EZDB_SECTION_COMPRESSION_LEVEL 3

enum {
    EZDB_OK = 0,
    EZDB_ERR_ARG = -1,
    EZDB_ERR_IO = -2,
    EZDB_ERR_FORMAT = -3,
    EZDB_ERR_MEMORY = -4,
    EZDB_ERR_NOT_FOUND = -5,
    EZDB_ERR_READ_ONLY = -6
};

typedef struct EzdbDiskPage {
    uint64_t offset;
    uint32_t encoded_size;
    uint32_t raw_size;
    uint32_t flags;
    uint32_t reserved;
} EzdbDiskPage;

typedef struct EzdbDeltaDiskHeader {
    uint32_t magic;
    uint32_t type;
    uint32_t id;
    uint32_t path_len;
    uint64_t size;
    uint64_t modified_time;
} EzdbDeltaDiskHeader;

typedef struct EzdbEntryDeltaDiskHeader {
    uint32_t magic;
    uint32_t type;
    uint32_t id;
    uint32_t archive_id;
    uint32_t entry_path_len;
    uint32_t entry_raw_path_len;
    int64_t compressed_size;
    uint64_t original_size;
    uint64_t modified_time;
} EzdbEntryDeltaDiskHeader;

typedef struct EzdbDiskFile {
    uint32_t parent_dir_id;
    uint32_t name_offset;
    uint32_t name_len;
    uint32_t flags;
    uint64_t size;
    uint64_t modified_time;
} EzdbDiskFile;

typedef struct EzdbDiskArchiveMeta {
    uint64_t file_ref_number;
    int64_t usn;
    unsigned char drive_letter;
    unsigned char reserved[7];
} EzdbDiskArchiveMeta;

typedef struct EzdbDiskEntry {
    uint32_t archive_id;
    uint32_t entry_path_offset;
    uint32_t entry_path_len;
    uint32_t raw_offset;
    uint32_t raw_len;
    uint32_t flags;
    int64_t compressed_size;
    uint64_t original_size;
    uint64_t modified_time;
} EzdbDiskEntry;

typedef struct EzdbDiskDir {
    uint32_t parent_dir_id;
    uint32_t name_offset;
    uint32_t name_len;
    uint32_t first_file_id;
    uint32_t file_count;
} EzdbDiskDir;

typedef struct EzdbDiskIndex {
    uint32_t key;
    uint32_t count;
    uint32_t container_type;
    uint32_t encoded_size;
    uint32_t raw_size;
    uint64_t offset;
} EzdbDiskIndex;

typedef struct EzdbDeltaEntryRef {
    uint64_t path_offset;
    uint32_t path_len;
    uint64_t raw_offset;
    uint32_t raw_len;
    int64_t compressed_size;
    uint64_t original_size;
    uint64_t modified_time;
} EzdbDeltaEntryRef;
