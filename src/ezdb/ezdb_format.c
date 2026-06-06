#include "ezdb_format.h"
#include "ezdb_internal.h"

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

static void ezdb_format_add_v13_section(EzdbSectionDesc* sections,
                                        uint32_t* section_count,
                                        uint32_t section_id,
                                        uint32_t flags,
                                        uint64_t offset,
                                        uint64_t encoded_size,
                                        uint64_t raw_size,
                                        uint64_t aux_offset,
                                        uint64_t aux_size,
                                        uint32_t page_size,
                                        uint32_t aux_count)
{
    if (!encoded_size && !raw_size && !aux_size && !aux_count) return;
    EzdbSectionDesc* section = &sections[*section_count];
    memset(section, 0, sizeof(*section));
    section->section_id = section_id;
    section->flags = flags;
    section->offset = offset;
    section->encoded_size = encoded_size;
    section->raw_size = raw_size;
    section->aux_offset = aux_offset;
    section->aux_size = aux_size;
    section->page_size = page_size;
    section->aux_count = aux_count;
    *section_count += 1u;
}

int ezdb_format_build_v13_sections_from_header(const EzdbHeader* header,
                                               EzdbSectionDesc* sections,
                                               uint32_t section_cap,
                                               uint32_t* out_section_count)
{
    if (!header || !sections || !out_section_count || section_cap < EZDB_SECTION_METADATA) return EZDB_FORMAT_ERR_ARG;
    uint32_t count = 0;
    ezdb_format_add_v13_section(sections, &count, EZDB_SECTION_ARCHIVE_RECORDS,
                                header->file_records_flags,
                                header->file_records_offset,
                                header->file_records_size,
                                header->file_records_raw_size,
                                0, 0, 0, 0);
    ezdb_format_add_v13_section(sections, &count, EZDB_SECTION_DIR_RECORDS,
                                header->dir_records_flags,
                                header->dir_records_offset,
                                header->dir_records_size,
                                header->dir_records_raw_size,
                                0, 0, 0, 0);
    ezdb_format_add_v13_section(sections, &count, EZDB_SECTION_STRING_POOL,
                                header->strings_flags,
                                header->strings_offset,
                                header->strings_size,
                                header->strings_raw_size,
                                0, 0, 0, 0);
    ezdb_format_add_v13_section(sections, &count, EZDB_SECTION_ARCHIVE_META,
                                header->archive_meta_flags,
                                header->archive_meta_offset,
                                header->archive_meta_size,
                                header->archive_meta_raw_size,
                                0, 0, 0, 0);
    ezdb_format_add_v13_section(sections, &count, EZDB_SECTION_ENTRY_CORE,
                                header->entry_records_flags,
                                header->entry_records_offset,
                                header->entry_records_size,
                                header->entry_records_raw_size,
                                0, 0, 0, 0);
    ezdb_format_add_v13_section(sections, &count, EZDB_SECTION_ENTRY_DETAIL_PAGES,
                                0,
                                header->entry_detail_offset,
                                header->entry_detail_size,
                                sizeof(EzdbDiskEntry) * header->base_entry_count,
                                header->entry_detail_index_offset,
                                sizeof(EzdbDiskPage) * header->entry_detail_page_count,
                                header->entry_page_size,
                                (uint32_t)header->entry_detail_page_count);
    ezdb_format_add_v13_section(sections, &count, EZDB_SECTION_ENTRY_RAW_PAGES,
                                0,
                                header->raw_blob_offset,
                                header->raw_blob_size,
                                header->raw_blob_raw_size,
                                header->raw_blob_index_offset,
                                sizeof(EzdbDiskPage) * header->raw_blob_page_count,
                                header->raw_blob_page_size,
                                (uint32_t)header->raw_blob_page_count);
    ezdb_format_add_v13_section(sections, &count, EZDB_SECTION_FILE_INDEX,
                                0,
                                header->file_index_offset,
                                sizeof(EzdbDiskIndex) * header->file_index_count,
                                sizeof(EzdbDiskIndex) * header->file_index_count,
                                0, 0, 0, 0);
    ezdb_format_add_v13_section(sections, &count, EZDB_SECTION_DIR_INDEX,
                                0,
                                header->dir_index_offset,
                                sizeof(EzdbDiskIndex) * header->dir_index_count,
                                sizeof(EzdbDiskIndex) * header->dir_index_count,
                                0, 0, 0, 0);
    ezdb_format_add_v13_section(sections, &count, EZDB_SECTION_ENTRY_INDEX,
                                0,
                                header->entry_index_offset,
                                sizeof(EzdbDiskIndex) * header->entry_index_count,
                                sizeof(EzdbDiskIndex) * header->entry_index_count,
                                0, 0, 0, 0);
    ezdb_format_add_v13_section(sections, &count, EZDB_SECTION_POSTINGS,
                                0,
                                header->postings_offset,
                                header->postings_size + header->entry_postings_size,
                                header->postings_size + header->entry_postings_size,
                                0, 0, 0, 0);
    ezdb_format_add_v13_section(sections, &count, EZDB_SECTION_DELTA_LOG,
                                0,
                                header->delta_offset,
                                header->delta_size,
                                header->delta_size,
                                0, 0, 0, 0);
    *out_section_count = count;
    return EZDB_FORMAT_OK;
}

int ezdb_format_write_v13_disk_header(FILE* fp,
                                      const EzdbHeader* header,
                                      uint32_t section_count,
                                      uint64_t section_table_offset)
{
    if (!fp || !header) return EZDB_FORMAT_ERR_ARG;
    EzdbV13Header disk_header;
    memset(&disk_header, 0, sizeof(disk_header));
    memcpy(disk_header.magic, EZDB_V13_MAGIC, sizeof(disk_header.magic));
    disk_header.version = EZDB_V13_VERSION;
    disk_header.header_size = sizeof(EzdbV13Header);
    disk_header.section_count = section_count;
    disk_header.section_table_offset = section_table_offset;
    disk_header.archive_count = header->file_count;
    disk_header.active_archive_count = header->active_count;
    disk_header.dir_count = header->dir_count;
    disk_header.entry_count = header->entry_count;
    disk_header.active_entry_count = header->active_entry_count;
    disk_header.base_archive_count = header->base_file_count;
    disk_header.base_entry_count = header->base_entry_count;
    if (fseek(fp, 0, SEEK_SET) != 0 || fwrite(&disk_header, sizeof(disk_header), 1, fp) != 1) {
        return EZDB_FORMAT_ERR_IO;
    }
    return EZDB_FORMAT_OK;
}

int ezdb_format_write_v13_header_and_section_table(FILE* fp,
                                                   const EzdbHeader* header,
                                                   uint64_t* out_table_offset,
                                                   uint64_t* out_table_size)
{
    if (!fp || !header) return EZDB_FORMAT_ERR_ARG;
    EzdbSectionDesc sections[EZDB_SECTION_METADATA];
    uint32_t section_count = 0;
    int rc = ezdb_format_build_v13_sections_from_header(header, sections, EZDB_SECTION_METADATA, &section_count);
    if (rc != EZDB_FORMAT_OK) return rc;
    uint64_t table_offset = 0;
    uint64_t table_size = 0;
    rc = ezdb_format_write_section_table(fp, sections, section_count, &table_offset, &table_size);
    if (rc != EZDB_FORMAT_OK) return rc;
    uint64_t file_size = table_offset + table_size;
    rc = ezdb_format_validate_section_table(sections, section_count, file_size);
    if (rc != EZDB_FORMAT_OK) return rc;
    rc = ezdb_format_write_v13_disk_header(fp, header, section_count, table_offset);
    if (rc != EZDB_FORMAT_OK) return rc;
    if (out_table_offset) *out_table_offset = table_offset;
    if (out_table_size) *out_table_size = table_size;
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
