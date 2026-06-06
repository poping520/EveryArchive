#include "ezdb_entries.h"
#include "ezdb_io.h"

#include <stdlib.h>
#include <string.h>

void ezdb_entries_page_cache_free(EzdbPageCacheEntry* cache, uint32_t count)
{
    if (!cache) return;
    for (uint32_t i = 0; i < count; ++i) {
        free(cache[i].data);
        memset(&cache[i], 0, sizeof(cache[i]));
    }
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
