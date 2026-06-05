#pragma once

#include "ezdb_internal.h"

#include <stdio.h>

int ezdb_io_write_bytes(FILE* fp, const void* data, uint32_t size, uint64_t* written);
int ezdb_io_maybe_compress_payload(const unsigned char* raw, uint32_t raw_size, unsigned char** out_data, uint32_t* out_size, int* out_compressed);
int ezdb_io_maybe_compress_section(const unsigned char* raw, uint64_t raw_size, unsigned char** out_data, uint64_t* out_size, uint32_t* out_flags);
int ezdb_io_write_compressed_section(FILE* out, const unsigned char* raw, uint64_t raw_size, uint64_t* out_written, uint32_t* out_flags);
int ezdb_io_read_section_payload(FILE* fp, uint64_t offset, uint64_t encoded_size, uint64_t raw_size, uint32_t flags, unsigned char** out_data);
int ezdb_io_read_section_into(FILE* fp, uint64_t offset, uint64_t encoded_size, uint64_t raw_size, uint32_t flags, unsigned char* out);
int ezdb_io_write_paged_section(FILE* out,
                                const unsigned char* raw,
                                uint64_t raw_size,
                                uint32_t page_size,
                                EzdbDiskPage** out_pages,
                                uint32_t* out_page_count,
                                uint64_t* out_written);

#define write_bytes ezdb_io_write_bytes
#define maybe_compress_payload ezdb_io_maybe_compress_payload
#define maybe_compress_section ezdb_io_maybe_compress_section
#define write_compressed_section ezdb_io_write_compressed_section
#define read_section_payload ezdb_io_read_section_payload
#define read_section_into ezdb_io_read_section_into
#define write_paged_section ezdb_io_write_paged_section
