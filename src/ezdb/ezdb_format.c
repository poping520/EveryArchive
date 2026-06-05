#include "ezdb_format.h"

#include <stdlib.h>
#include <string.h>

int ezdb_format_header_is_current(const EzdbHeader* header)
{
    if (!header) return 0;
    return memcmp(header->magic, EZDB_MAGIC, sizeof(header->magic)) == 0 &&
           header->version == EZDB_VERSION &&
           header->header_size == sizeof(EzdbHeader);
}

int ezdb_format_v13_header_is_current(const EzdbV13Header* header)
{
    if (!header) return 0;
    return memcmp(header->magic, EZDB_V13_MAGIC, sizeof(header->magic)) == 0 &&
           header->version == EZDB_V13_VERSION &&
           header->header_size == sizeof(EzdbV13Header);
}

int ezdb_format_section_id_is_known(uint32_t section_id)
{
    return section_id >= EZDB_SECTION_ARCHIVE_RECORDS && section_id <= EZDB_SECTION_METADATA;
}

static int ezdb_format_section_compare(const void* a, const void* b)
{
    const EzdbSectionDesc* sa = (const EzdbSectionDesc*)a;
    const EzdbSectionDesc* sb = (const EzdbSectionDesc*)b;
    if (sa->section_id == sb->section_id) return 0;
    return sa->section_id < sb->section_id ? -1 : 1;
}

typedef struct EzdbSectionRange {
    uint64_t offset;
    uint64_t size;
} EzdbSectionRange;

static int ezdb_format_section_range_compare(const void* a, const void* b)
{
    const EzdbSectionRange* ra = (const EzdbSectionRange*)a;
    const EzdbSectionRange* rb = (const EzdbSectionRange*)b;
    if (ra->offset == rb->offset) {
        if (ra->size == rb->size) return 0;
        return ra->size < rb->size ? -1 : 1;
    }
    return ra->offset < rb->offset ? -1 : 1;
}

int ezdb_format_validate_section_table(const EzdbSectionDesc* sections, uint32_t section_count, uint64_t file_size)
{
    if (!sections && section_count) return EZDB_FORMAT_ERR_ARG;
    if (!section_count) return EZDB_FORMAT_ERR_FORMAT;

    EzdbSectionDesc* by_id = (EzdbSectionDesc*)malloc(sizeof(EzdbSectionDesc) * (size_t)section_count);
    EzdbSectionRange* ranges = (EzdbSectionRange*)malloc(sizeof(EzdbSectionRange) * (size_t)section_count * 2u);
    if (!by_id || !ranges) {
        free(by_id);
        free(ranges);
        return EZDB_FORMAT_ERR_MEMORY;
    }
    memcpy(by_id, sections, sizeof(EzdbSectionDesc) * (size_t)section_count);
    qsort(by_id, section_count, sizeof(EzdbSectionDesc), ezdb_format_section_compare);

    uint32_t range_count = 0;
    for (uint32_t i = 0; i < section_count; ++i) {
        const EzdbSectionDesc* section = &by_id[i];
        if (!ezdb_format_section_id_is_known(section->section_id)) {
            free(by_id);
            free(ranges);
            return EZDB_FORMAT_ERR_FORMAT;
        }
        if (i && by_id[i - 1u].section_id == section->section_id) {
            free(by_id);
            free(ranges);
            return EZDB_FORMAT_ERR_FORMAT;
        }
        if (section->encoded_size > file_size || section->offset > file_size - section->encoded_size) {
            free(by_id);
            free(ranges);
            return EZDB_FORMAT_ERR_FORMAT;
        }
        if (!section->encoded_size && section->raw_size) {
            free(by_id);
            free(ranges);
            return EZDB_FORMAT_ERR_FORMAT;
        }
        if (section->aux_size > file_size || section->aux_offset > file_size - section->aux_size) {
            free(by_id);
            free(ranges);
            return EZDB_FORMAT_ERR_FORMAT;
        }
        if (!section->aux_size && section->aux_count) {
            free(by_id);
            free(ranges);
            return EZDB_FORMAT_ERR_FORMAT;
        }
        if (section->encoded_size) {
            ranges[range_count].offset = section->offset;
            ranges[range_count].size = section->encoded_size;
            ++range_count;
        }
        if (section->aux_size) {
            ranges[range_count].offset = section->aux_offset;
            ranges[range_count].size = section->aux_size;
            ++range_count;
        }
    }

    qsort(ranges, range_count, sizeof(EzdbSectionRange), ezdb_format_section_range_compare);
    for (uint32_t i = 1; i < range_count; ++i) {
        const EzdbSectionRange* prev = &ranges[i - 1u];
        const EzdbSectionRange* cur = &ranges[i];
        if (cur->offset < prev->offset + prev->size) {
            free(by_id);
            free(ranges);
            return EZDB_FORMAT_ERR_FORMAT;
        }
    }

    free(by_id);
    free(ranges);
    return EZDB_FORMAT_OK;
}

const EzdbSectionDesc* ezdb_format_find_section(const EzdbSectionDesc* sections, uint32_t section_count, uint32_t section_id)
{
    if (!sections) return NULL;
    for (uint32_t i = 0; i < section_count; ++i) {
        if (sections[i].section_id == section_id) return &sections[i];
    }
    return NULL;
}

int ezdb_format_write_section_table(FILE* fp,
                                    const EzdbSectionDesc* sections,
                                    uint32_t section_count,
                                    uint64_t* out_offset,
                                    uint64_t* out_size)
{
    if (!fp || (!sections && section_count)) return EZDB_FORMAT_ERR_ARG;
    long pos = ftell(fp);
    if (pos < 0) return EZDB_FORMAT_ERR_IO;
    uint64_t offset = (uint64_t)pos;
    if (section_count && fwrite(sections, sizeof(EzdbSectionDesc), (size_t)section_count, fp) != (size_t)section_count) {
        return EZDB_FORMAT_ERR_IO;
    }
    if (out_offset) *out_offset = offset;
    if (out_size) *out_size = sizeof(EzdbSectionDesc) * (uint64_t)section_count;
    return EZDB_FORMAT_OK;
}

int ezdb_format_read_section_table(FILE* fp,
                                   const EzdbV13Header* header,
                                   uint64_t file_size,
                                   EzdbSectionDesc** out_sections)
{
    if (!fp || !header || !out_sections) return EZDB_FORMAT_ERR_ARG;
    *out_sections = NULL;
    if (!ezdb_format_v13_header_is_current(header)) return EZDB_FORMAT_ERR_FORMAT;
    if (!header->section_count) return EZDB_FORMAT_ERR_FORMAT;
    uint64_t table_size = sizeof(EzdbSectionDesc) * (uint64_t)header->section_count;
    if (table_size > file_size || header->section_table_offset > file_size - table_size) {
        return EZDB_FORMAT_ERR_FORMAT;
    }
    EzdbSectionDesc* sections = (EzdbSectionDesc*)malloc(sizeof(EzdbSectionDesc) * (size_t)header->section_count);
    if (!sections) return EZDB_FORMAT_ERR_MEMORY;
    if (fseek(fp, (long)header->section_table_offset, SEEK_SET) != 0 ||
        fread(sections, sizeof(EzdbSectionDesc), (size_t)header->section_count, fp) != (size_t)header->section_count) {
        free(sections);
        return EZDB_FORMAT_ERR_IO;
    }
    int rc = ezdb_format_validate_section_table(sections, header->section_count, file_size);
    if (rc != EZDB_FORMAT_OK) {
        free(sections);
        return rc;
    }
    *out_sections = sections;
    return EZDB_FORMAT_OK;
}
