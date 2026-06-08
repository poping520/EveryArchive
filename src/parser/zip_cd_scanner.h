#pragma once

#include <stdint.h>
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ZipCdEntry {
    const char* raw_name;
    uint32_t raw_name_len;
    uint16_t flags;
    int64_t compressed_size;
    uint64_t original_size;
    uint64_t modified_time;
} ZipCdEntry;

typedef int (*ZipCdEntryCallback)(const ZipCdEntry* entry, void* user_data);

int zip_cd_scan_entries(const wchar_t* path, ZipCdEntryCallback callback, void* user_data, char* error, uint32_t error_size);

#ifdef __cplusplus
}
#endif
