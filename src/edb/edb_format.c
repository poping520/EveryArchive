#include "edb_format.h"
#include "edb_util.h"
#include <zlib.h>
#include <stdlib.h>
#include <string.h>

/* ===== LE 读写 ===== */

static void edb_write_le32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void edb_write_le64(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (i * 8));
}

static uint32_t edb_read_le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t edb_read_le64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)p[i] << (i * 8);
    return v;
}

/* ===== Header 读写 ===== */

int edb_format_write_header(FILE* fp, const EdbHeader* hdr) {
    uint8_t buf[EDB_HEADER_SIZE];
    memset(buf, 0, EDB_HEADER_SIZE);
    int off = 0;

    memcpy(buf + off, hdr->magic, 8); off += 8;
    edb_write_le32(buf + off, hdr->version); off += 4;
    edb_write_le32(buf + off, hdr->header_size); off += 4;
    edb_write_le32(buf + off, hdr->flags); off += 4;
    edb_write_le32(buf + off, hdr->section_count); off += 4;
    edb_write_le64(buf + off, hdr->archive_count); off += 8;
    edb_write_le64(buf + off, hdr->active_archive_count); off += 8;
    edb_write_le64(buf + off, hdr->entry_count); off += 8;
    edb_write_le64(buf + off, hdr->active_entry_count); off += 8;
    edb_write_le64(buf + off, hdr->section_table_offset); off += 8;

    uint32_t crc = edb_fnv1a(buf, 64);
    edb_write_le32(buf + off, crc); off += 4;

    if (fseek(fp, 0, SEEK_SET) != 0) return EDB_ERR_IO;
    if (fwrite(buf, 1, EDB_HEADER_SIZE, fp) != EDB_HEADER_SIZE) return EDB_ERR_IO;
    return EDB_OK;
}

int edb_format_read_header(FILE* fp, uint64_t file_size, EdbHeader* out) {
    uint8_t buf[EDB_HEADER_SIZE];
    if (file_size < EDB_HEADER_SIZE) return EDB_ERR_FORMAT;
    if (fseek(fp, 0, SEEK_SET) != 0) return EDB_ERR_IO;
    if (fread(buf, 1, EDB_HEADER_SIZE, fp) != EDB_HEADER_SIZE) return EDB_ERR_IO;

    memcpy(out->magic, buf, 8);
    if (memcmp(out->magic, EDB_MAGIC, 8) != 0) return EDB_ERR_FORMAT;

    out->version = edb_read_le32(buf + 8);
    if (out->version != EDB_VERSION) return EDB_ERR_FORMAT;

    out->header_size = edb_read_le32(buf + 12);
    out->flags = edb_read_le32(buf + 16);
    out->section_count = edb_read_le32(buf + 20);
    out->archive_count = edb_read_le64(buf + 24);
    out->active_archive_count = edb_read_le64(buf + 32);
    out->entry_count = edb_read_le64(buf + 40);
    out->active_entry_count = edb_read_le64(buf + 48);
    out->section_table_offset = edb_read_le64(buf + 56);
    out->checksum = edb_read_le32(buf + 64);

    if (out->section_count == 0 || out->section_table_offset == 0) return EDB_ERR_FORMAT;
    if (out->section_table_offset + (uint64_t)out->section_count * EDB_SECTION_DESC_SIZE > file_size)
        return EDB_ERR_FORMAT;

    return EDB_OK;
}

/* ===== Section Table 读写 ===== */

int edb_format_write_section_table(FILE* fp, const EdbSectionDesc* sections,
                                    uint32_t count, uint64_t* out_offset) {
    uint8_t buf[EDB_SECTION_DESC_SIZE];

    long pos = ftell(fp);
    if (pos < 0) return EDB_ERR_IO;
    *out_offset = (uint64_t)pos;

    for (uint32_t i = 0; i < count; i++) {
        memset(buf, 0, EDB_SECTION_DESC_SIZE);
        int off = 0;
        edb_write_le32(buf + off, sections[i].section_id); off += 4;
        edb_write_le32(buf + off, sections[i].flags); off += 4;
        edb_write_le64(buf + off, sections[i].offset); off += 8;
        edb_write_le64(buf + off, sections[i].encoded_size); off += 8;
        edb_write_le64(buf + off, sections[i].raw_size); off += 8;
        edb_write_le64(buf + off, sections[i].aux_offset); off += 8;
        edb_write_le32(buf + off, sections[i].aux_count); off += 4;
        edb_write_le32(buf + off, sections[i].page_size); off += 4;

        if (fwrite(buf, 1, EDB_SECTION_DESC_SIZE, fp) != EDB_SECTION_DESC_SIZE)
            return EDB_ERR_IO;
    }
    return EDB_OK;
}

int edb_format_read_section_table(FILE* fp, uint64_t table_offset, uint32_t count,
                                   EdbSectionDesc* out) {
    uint8_t buf[EDB_SECTION_DESC_SIZE];
    if (fseek(fp, (long)table_offset, SEEK_SET) != 0) return EDB_ERR_IO;

    for (uint32_t i = 0; i < count; i++) {
        if (fread(buf, 1, EDB_SECTION_DESC_SIZE, fp) != EDB_SECTION_DESC_SIZE)
            return EDB_ERR_IO;
        int off = 0;
        out[i].section_id = edb_read_le32(buf + off); off += 4;
        out[i].flags = edb_read_le32(buf + off); off += 4;
        out[i].offset = edb_read_le64(buf + off); off += 8;
        out[i].encoded_size = edb_read_le64(buf + off); off += 8;
        out[i].raw_size = edb_read_le64(buf + off); off += 8;
        out[i].aux_offset = edb_read_le64(buf + off); off += 8;
        out[i].aux_count = edb_read_le32(buf + off); off += 4;
        out[i].page_size = edb_read_le32(buf + off); off += 4;
        out[i].reserved = 0;
    }
    return EDB_OK;
}

const EdbSectionDesc* edb_format_find_section(const EdbSectionDesc* sections,
                                               uint32_t count, uint32_t id) {
    for (uint32_t i = 0; i < count; i++) {
        if (sections[i].section_id == id) return &sections[i];
    }
    return NULL;
}

/* ===== 压缩 ===== */

int edb_format_compress(const uint8_t* src, uint32_t src_len,
                         uint8_t** out, uint32_t* out_len) {
    if (src_len < EDB_COMPRESS_MIN_SIZE) return -1;

    uLong bound = compressBound((uLong)src_len);
    uint8_t* dst = (uint8_t*)malloc(bound);
    if (!dst) return EDB_ERR_MEMORY;

    uLong dlen = bound;
    int rc = compress2(dst, &dlen, src, (uLong)src_len, EDB_COMPRESSION_LEVEL);
    if (rc != Z_OK) { free(dst); return -1; }

    if ((uLong)src_len - dlen < EDB_COMPRESS_MIN_SAVING) {
        free(dst);
        return -1;
    }

    *out = dst;
    *out_len = (uint32_t)dlen;
    return 0;
}

int edb_format_decompress(const uint8_t* src, uint32_t src_len,
                           uint8_t* out, uint32_t out_cap, uint32_t* out_len) {
    uLong dlen = (uLong)out_cap;
    int rc = uncompress(out, &dlen, src, (uLong)src_len);
    if (rc != Z_OK) return EDB_ERR_FORMAT;
    *out_len = (uint32_t)dlen;
    return EDB_OK;
}

/* ===== 分页缓存 ===== */

void edb_page_cache_init(EdbPageCache* cache) {
    memset(cache, 0, sizeof(*cache));
}

void edb_page_cache_free(EdbPageCache* cache) {
    for (int i = 0; i < EDB_CACHE_SLOTS; i++) {
        if (cache->slots[i].valid && cache->slots[i].data) {
            free(cache->slots[i].data);
        }
    }
    memset(cache, 0, sizeof(*cache));
}

void* edb_page_cache_get(EdbPageCache* cache, uint32_t page_id) {
    for (int i = 0; i < EDB_CACHE_SLOTS; i++) {
        if (cache->slots[i].valid && cache->slots[i].page_id == page_id) {
            cache->slots[i].tick = ++cache->tick_counter;
            return cache->slots[i].data;
        }
    }
    return NULL;
}

void edb_page_cache_put(EdbPageCache* cache, uint32_t page_id, void* data, uint32_t size) {
    (void)size;
    int victim = 0;
    uint32_t min_tick = UINT32_MAX;
    for (int i = 0; i < EDB_CACHE_SLOTS; i++) {
        if (!cache->slots[i].valid) { victim = i; break; }
        if (cache->slots[i].tick < min_tick) {
            min_tick = cache->slots[i].tick;
            victim = i;
        }
    }
    if (cache->slots[victim].valid && cache->slots[victim].data) {
        free(cache->slots[victim].data);
    }
    cache->slots[victim].page_id = page_id;
    cache->slots[victim].data = data;
    cache->slots[victim].tick = ++cache->tick_counter;
    cache->slots[victim].valid = 1;
}

/* ===== 分页读取 ===== */

int edb_format_read_page(FILE* fp, const EdbDiskPage* pages, uint32_t page_idx,
                          EdbPageCache* cache, void** out_data, uint32_t* out_size) {
    void* cached = edb_page_cache_get(cache, page_idx);
    if (cached) {
        *out_data = cached;
        *out_size = pages[page_idx].raw_size;
        return EDB_OK;
    }

    const EdbDiskPage* pg = &pages[page_idx];
    if (fseek(fp, (long)pg->offset, SEEK_SET) != 0) return EDB_ERR_IO;

    uint8_t* raw = NULL;

    if (pg->flags & EDB_SECTION_COMPRESSED) {
        uint8_t* compressed = (uint8_t*)malloc(pg->encoded_size);
        if (!compressed) return EDB_ERR_MEMORY;
        if (fread(compressed, 1, pg->encoded_size, fp) != pg->encoded_size) {
            free(compressed); return EDB_ERR_IO;
        }
        raw = (uint8_t*)malloc(pg->raw_size);
        if (!raw) { free(compressed); return EDB_ERR_MEMORY; }
        uint32_t dec_len = 0;
        int rc = edb_format_decompress(compressed, pg->encoded_size,
                                        raw, pg->raw_size, &dec_len);
        free(compressed);
        if (rc != EDB_OK) { free(raw); return rc; }
    } else {
        raw = (uint8_t*)malloc(pg->raw_size);
        if (!raw) return EDB_ERR_MEMORY;
        if (fread(raw, 1, pg->raw_size, fp) != pg->raw_size) {
            free(raw); return EDB_ERR_IO;
        }
    }

    edb_page_cache_put(cache, page_idx, raw, pg->raw_size);
    *out_data = raw;
    *out_size = pg->raw_size;
    return EDB_OK;
}

int edb_format_read_entry_detail(FILE* fp, EdbDiskPage* pages, uint32_t page_count,
                                  EdbPageCache* cache, uint32_t entry_id,
                                  EdbDiskEntryDetail* out) {
    uint32_t page_idx = entry_id / EDB_DETAIL_ENTRIES_PER_PAGE;
    uint32_t slot = entry_id % EDB_DETAIL_ENTRIES_PER_PAGE;
    if (page_idx >= page_count) return EDB_ERR_NOT_FOUND;

    void* data = NULL;
    uint32_t data_size = 0;
    int rc = edb_format_read_page(fp, pages, page_idx, cache, &data, &data_size);
    if (rc != EDB_OK) return rc;

    uint32_t elem = (uint32_t)sizeof(EdbDiskEntryDetail);
    if (slot * elem + elem > data_size) return EDB_ERR_FORMAT;
    memcpy(out, (uint8_t*)data + slot * elem, elem);
    return EDB_OK;
}

int edb_format_read_entry_path(FILE* fp, EdbDiskPage* pages, uint32_t page_count,
                                EdbPageCache* cache,
                                uint32_t path_offset, uint32_t path_len,
                                char** out_path) {
    /* path_offset 是跨页的逻辑偏移，需要定位到具体页 */
    uint32_t accum = 0;
    for (uint32_t i = 0; i < page_count; i++) {
        uint32_t pg_raw = pages[i].raw_size;
        if (path_offset < accum + pg_raw) {
            uint32_t in_page = path_offset - accum;
            void* data = NULL;
            uint32_t data_size = 0;
            int rc = edb_format_read_page(fp, pages, i, cache, &data, &data_size);
            if (rc != EDB_OK) return rc;
            if (in_page + path_len > data_size) return EDB_ERR_FORMAT;
            *out_path = (char*)malloc(path_len + 1);
            if (!*out_path) return EDB_ERR_MEMORY;
            memcpy(*out_path, (char*)data + in_page, path_len);
            (*out_path)[path_len] = '\0';
            return EDB_OK;
        }
        accum += pg_raw;
    }
    return EDB_ERR_NOT_FOUND;
}

int edb_format_read_entry_raw(FILE* fp, EdbDiskPage* pages, uint32_t page_count,
                               EdbPageCache* cache,
                               uint32_t raw_offset, uint32_t raw_len,
                               void** out_raw) {
    if (raw_len == 0 || raw_offset == UINT32_MAX) {
        *out_raw = NULL;
        return EDB_OK;
    }
    uint32_t accum = 0;
    for (uint32_t i = 0; i < page_count; i++) {
        uint32_t pg_raw = pages[i].raw_size;
        if (raw_offset < accum + pg_raw) {
            uint32_t in_page = raw_offset - accum;
            void* data = NULL;
            uint32_t data_size = 0;
            int rc = edb_format_read_page(fp, pages, i, cache, &data, &data_size);
            if (rc != EDB_OK) return rc;
            if (in_page + raw_len > data_size) return EDB_ERR_FORMAT;
            *out_raw = malloc(raw_len);
            if (!*out_raw) return EDB_ERR_MEMORY;
            memcpy(*out_raw, (uint8_t*)data + in_page, raw_len);
            return EDB_OK;
        }
        accum += pg_raw;
    }
    return EDB_ERR_NOT_FOUND;
}
