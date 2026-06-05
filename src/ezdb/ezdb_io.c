#include "ezdb_io.h"

#include <zlib.h>

#include <limits.h>
#include <stdlib.h>
#include <string.h>

int ezdb_io_write_bytes(FILE* fp, const void* data, uint32_t size, uint64_t* written)
{
    if (!fp || !written || (!data && size)) return EZDB_ERR_ARG;
    if (size && fwrite(data, 1, size, fp) != size) return EZDB_ERR_IO;
    *written += size;
    return EZDB_OK;
}

int ezdb_io_maybe_compress_payload(const unsigned char* raw, uint32_t raw_size, unsigned char** out_data, uint32_t* out_size, int* out_compressed)
{
    if (!out_data || !out_size || !out_compressed || (!raw && raw_size)) return EZDB_ERR_ARG;
    *out_data = NULL;
    *out_size = 0;
    *out_compressed = 0;
    if (raw_size >= EZDB_POSTING_COMPRESS_MIN_SIZE) {
        uLongf bound = compressBound((uLong)raw_size);
        if (bound <= UINT32_MAX) {
            unsigned char* compressed = (unsigned char*)malloc((size_t)bound);
            if (!compressed) return EZDB_ERR_MEMORY;
            uLongf compressed_size = bound;
            int zrc = compress2(compressed, &compressed_size, raw, (uLong)raw_size, EZDB_POSTING_COMPRESSION_LEVEL);
            if (zrc == Z_OK && compressed_size + EZDB_POSTING_COMPRESS_MIN_SAVING < raw_size && compressed_size <= UINT32_MAX) {
                *out_data = compressed;
                *out_size = (uint32_t)compressed_size;
                *out_compressed = 1;
                return EZDB_OK;
            }
            free(compressed);
        }
    }
    unsigned char* copy = (unsigned char*)malloc(raw_size ? raw_size : 1u);
    if (!copy) return EZDB_ERR_MEMORY;
    if (raw_size) memcpy(copy, raw, raw_size);
    *out_data = copy;
    *out_size = raw_size;
    return EZDB_OK;
}

int ezdb_io_maybe_compress_section(const unsigned char* raw, uint64_t raw_size, unsigned char** out_data, uint64_t* out_size, uint32_t* out_flags)
{
    if (!out_data || !out_size || !out_flags || (!raw && raw_size)) return EZDB_ERR_ARG;
    *out_data = NULL;
    *out_size = 0;
    *out_flags = 0;
    if (raw_size > UINT32_MAX) return EZDB_ERR_MEMORY;
    if (raw_size >= EZDB_SECTION_COMPRESS_MIN_SIZE) {
        uLongf bound = compressBound((uLong)raw_size);
        if (bound <= UINT32_MAX) {
            unsigned char* compressed = (unsigned char*)malloc((size_t)bound);
            if (!compressed) return EZDB_ERR_MEMORY;
            uLongf compressed_size = bound;
            int zrc = compress2(compressed, &compressed_size, raw, (uLong)raw_size, EZDB_SECTION_COMPRESSION_LEVEL);
            if (zrc == Z_OK && compressed_size + EZDB_SECTION_COMPRESS_MIN_SAVING < raw_size) {
                *out_data = compressed;
                *out_size = (uint64_t)compressed_size;
                *out_flags = EZDB_SECTION_COMPRESSED;
                return EZDB_OK;
            }
            free(compressed);
        }
    }
    unsigned char* copy = (unsigned char*)malloc(raw_size ? (size_t)raw_size : 1u);
    if (!copy) return EZDB_ERR_MEMORY;
    if (raw_size) memcpy(copy, raw, (size_t)raw_size);
    *out_data = copy;
    *out_size = raw_size;
    return EZDB_OK;
}

int ezdb_io_write_compressed_section(FILE* out, const unsigned char* raw, uint64_t raw_size, uint64_t* out_written, uint32_t* out_flags)
{
    if (!out || !out_written || !out_flags || (!raw && raw_size)) return EZDB_ERR_ARG;
    unsigned char* payload = NULL;
    uint64_t payload_size = 0;
    int rc = ezdb_io_maybe_compress_section(raw, raw_size, &payload, &payload_size, out_flags);
    if (rc != EZDB_OK) return rc;
    if (payload_size && fwrite(payload, 1, (size_t)payload_size, out) != (size_t)payload_size) rc = EZDB_ERR_IO;
    free(payload);
    *out_written = payload_size;
    return rc;
}

int ezdb_io_read_section_payload(FILE* fp, uint64_t offset, uint64_t encoded_size, uint64_t raw_size, uint32_t flags, unsigned char** out_data)
{
    if (!fp || !out_data) return EZDB_ERR_ARG;
    if (encoded_size > UINT32_MAX || raw_size > UINT32_MAX) return EZDB_ERR_MEMORY;
    unsigned char* encoded = (unsigned char*)malloc(encoded_size ? (size_t)encoded_size : 1u);
    if (!encoded) return EZDB_ERR_MEMORY;
    if (fseek(fp, (long)offset, SEEK_SET) != 0 ||
        (encoded_size && fread(encoded, 1, (size_t)encoded_size, fp) != (size_t)encoded_size)) {
        free(encoded);
        return EZDB_ERR_IO;
    }
    if (!(flags & EZDB_SECTION_COMPRESSED)) {
        *out_data = encoded;
        return EZDB_OK;
    }
    unsigned char* raw = (unsigned char*)malloc(raw_size ? (size_t)raw_size : 1u);
    if (!raw) {
        free(encoded);
        return EZDB_ERR_MEMORY;
    }
    uLongf dest_len = (uLongf)raw_size;
    int zrc = uncompress(raw, &dest_len, encoded, (uLong)encoded_size);
    free(encoded);
    if (zrc != Z_OK || dest_len != raw_size) {
        free(raw);
        return EZDB_ERR_FORMAT;
    }
    *out_data = raw;
    return EZDB_OK;
}

int ezdb_io_read_section_into(FILE* fp, uint64_t offset, uint64_t encoded_size, uint64_t raw_size, uint32_t flags, unsigned char* out)
{
    if (!fp || (!out && raw_size)) return EZDB_ERR_ARG;
    if (encoded_size > UINT32_MAX || raw_size > UINT32_MAX) return EZDB_ERR_MEMORY;
    if (!(flags & EZDB_SECTION_COMPRESSED)) {
        if (fseek(fp, (long)offset, SEEK_SET) != 0 ||
            (raw_size && fread(out, 1, (size_t)raw_size, fp) != (size_t)raw_size)) {
            return EZDB_ERR_IO;
        }
        return EZDB_OK;
    }
    unsigned char* encoded = (unsigned char*)malloc(encoded_size ? (size_t)encoded_size : 1u);
    if (!encoded) return EZDB_ERR_MEMORY;
    if (fseek(fp, (long)offset, SEEK_SET) != 0 ||
        (encoded_size && fread(encoded, 1, (size_t)encoded_size, fp) != (size_t)encoded_size)) {
        free(encoded);
        return EZDB_ERR_IO;
    }
    uLongf dest_len = (uLongf)raw_size;
    int zrc = uncompress(out, &dest_len, encoded, (uLong)encoded_size);
    free(encoded);
    if (zrc != Z_OK || dest_len != raw_size) return EZDB_ERR_FORMAT;
    return EZDB_OK;
}

int ezdb_io_write_paged_section(FILE* out,
                                const unsigned char* raw,
                                uint64_t raw_size,
                                uint32_t page_size,
                                EzdbDiskPage** out_pages,
                                uint32_t* out_page_count,
                                uint64_t* out_written)
{
    if (!out || (!raw && raw_size) || !out_pages || !out_page_count || !out_written) return EZDB_ERR_ARG;
    *out_pages = NULL;
    *out_page_count = 0;
    *out_written = 0;
    if (!page_size) return EZDB_ERR_ARG;
    uint32_t page_count = (uint32_t)((raw_size + page_size - 1u) / page_size);
    if (!page_count) return EZDB_OK;
    EzdbDiskPage* pages = (EzdbDiskPage*)calloc(page_count, sizeof(EzdbDiskPage));
    if (!pages) return EZDB_ERR_MEMORY;
    uint64_t written = 0;
    int rc = EZDB_OK;
    for (uint32_t i = 0; i < page_count; ++i) {
        uint64_t page_offset = (uint64_t)i * page_size;
        uint32_t page_raw_size = (raw_size - page_offset) > page_size ? page_size : (uint32_t)(raw_size - page_offset);
        unsigned char* payload = NULL;
        uint64_t payload_size = 0;
        uint32_t flags = 0;
        rc = ezdb_io_maybe_compress_section(raw + page_offset, page_raw_size, &payload, &payload_size, &flags);
        if (rc != EZDB_OK) break;
        pages[i].offset = written;
        pages[i].encoded_size = (uint32_t)payload_size;
        pages[i].raw_size = page_raw_size;
        pages[i].flags = flags;
        if (payload_size && fwrite(payload, 1, (size_t)payload_size, out) != (size_t)payload_size) rc = EZDB_ERR_IO;
        free(payload);
        if (rc != EZDB_OK) break;
        written += payload_size;
    }
    if (rc != EZDB_OK) {
        free(pages);
        return rc;
    }
    *out_pages = pages;
    *out_page_count = page_count;
    *out_written = written;
    return EZDB_OK;
}
