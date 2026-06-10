#include "edb.h"
#include "edb_format.h"
#include "edb_build.h"
#include "edb_postings.h"
#include "edb_search.h"
#include "edb_util.h"
#include <stdlib.h>
#include <string.h>

/* ===== 内部类型 ===== */

typedef struct {
    uint32_t name_offset;
    uint32_t name_len;
    uint64_t file_size;
    uint64_t modified_time;
    char     drive_letter;
    uint64_t file_ref_number;
    int64_t  usn;
} EdbArchiveInMem;

typedef struct {
    uint32_t archive_id;
    uint32_t path_offset;
    uint32_t path_len;
} EdbEntryCoreInMem;

/* 3-phase entry replace 状态 */
typedef struct EdbEntryReplace {
    uint32_t archive_id;
    unsigned char* saved_bits;
    uint32_t       saved_bits_len;
    EdbEntryCoreInMem* new_cores;
    uint32_t           new_core_count;
    uint32_t           new_core_cap;
    /* entry detail 数据 */
    EdbDiskEntryDetail* new_details;
    /* entry path 数据 */
    char*     new_paths;
    uint32_t  new_paths_size;
    uint32_t  new_paths_cap;
} EdbEntryReplace;

struct Edb {
    FILE*  fp;
    char*  path;
    int    read_only;

    /* 压缩包数据（全内存） */
    EdbArchiveInMem* archives;
    uint32_t         archive_count;
    uint32_t         archive_cap;
    char*            archive_strings;
    uint64_t         archive_strings_size;

    /* 条目核心（全内存） */
    EdbEntryCoreInMem* entries;
    uint32_t           entry_count;
    uint32_t           entry_cap;

    /* 条目详情分页 */
    EdbDiskPage*    detail_pages;
    uint32_t        detail_page_count;
    EdbPageCache    detail_cache;

    /* 条目路径分页 */
    EdbDiskPage*    path_pages;
    uint32_t        path_page_count;
    EdbPageCache    path_cache;

    /* 原始路径分页 */
    EdbDiskPage*    raw_pages;
    uint32_t        raw_page_count;
    EdbPageCache    raw_cache;

    /* 活跃位图 */
    unsigned char*  active_archive_bits;
    unsigned char*  active_entry_bits;

    /* 倒排索引（全内存） */
    EdbDiskIndex*   archive_index;
    uint32_t        archive_index_count;
    unsigned char*  archive_postings;
    uint64_t        archive_postings_size;

    EdbDiskIndex*   entry_index;
    uint32_t        entry_index_count;
    unsigned char*  entry_postings;
    uint64_t        entry_postings_size;

    /* Archive by ref 哈希表 */
    uint32_t*       ref_hash_buckets;
    uint32_t        ref_hash_cap;

    /* 3-phase replace */
    EdbEntryReplace* pending_replace;

    /* 事务 */
    int             txn_active;
    uint32_t        txn_save_entry_count;
    unsigned char*  txn_save_active_entry_bits;
    uint32_t        txn_save_bits_size;

    /* 元数据 */
    char*           meta_path;

    /* section 描述符 */
    EdbSectionDesc  sections[EDB_SEC_COUNT];
};

/* ===== 辅助函数 ===== */

static uint32_t edb_bits_bytes(uint32_t count) {
    return (count + 7) / 8;
}

static int edb_load_section_data(FILE* fp, const EdbSectionDesc* sec,
                                  uint8_t** out, uint32_t* out_len) {
    if (sec->raw_size == 0) { *out = NULL; *out_len = 0; return EDB_OK; }
    if (fseek(fp, (long)sec->offset, SEEK_SET) != 0) return EDB_ERR_IO;

    if (sec->flags & EDB_SECTION_COMPRESSED) {
        uint8_t* comp = (uint8_t*)malloc((size_t)sec->encoded_size);
        if (!comp) return EDB_ERR_MEMORY;
        if (fread(comp, 1, (size_t)sec->encoded_size, fp) != (size_t)sec->encoded_size) {
            free(comp); return EDB_ERR_IO;
        }
        *out = (uint8_t*)malloc((size_t)sec->raw_size);
        if (!*out) { free(comp); return EDB_ERR_MEMORY; }
        uint32_t dec = 0;
        int rc = edb_format_decompress(comp, (uint32_t)sec->encoded_size,
                                        *out, (uint32_t)sec->raw_size, &dec);
        free(comp);
        if (rc != EDB_OK) { free(*out); return rc; }
        *out_len = dec;
    } else {
        *out = (uint8_t*)malloc((size_t)sec->raw_size);
        if (!*out) return EDB_ERR_MEMORY;
        if (fread(*out, 1, (size_t)sec->raw_size, fp) != (size_t)sec->raw_size) {
            free(*out); return EDB_ERR_IO;
        }
        *out_len = (uint32_t)sec->raw_size;
    }
    return EDB_OK;
}

static void edb_build_ref_hash(Edb* db) {
    uint32_t cap = db->archive_count * 2 + 64;
    db->ref_hash_buckets = (uint32_t*)calloc(cap, sizeof(uint32_t));
    db->ref_hash_cap = cap;

    for (uint32_t i = 0; i < db->archive_count; i++) {
        uint64_t key = ((uint64_t)(unsigned char)db->archives[i].drive_letter << 56) |
                       db->archives[i].file_ref_number;
        uint32_t h = edb_murmur3_final((uint32_t)key ^ (uint32_t)(key >> 32));
        uint32_t idx = h % cap;
        while (db->ref_hash_buckets[idx] != 0)
            idx = (idx + 1) % cap;
        db->ref_hash_buckets[idx] = i + 1; /* 1-based */
    }
}

static uint32_t edb_find_archive_by_ref(Edb* db, char drive_letter, uint64_t file_ref_number) {
    if (!db->ref_hash_buckets) return 0;
    uint64_t key = ((uint64_t)(unsigned char)drive_letter << 56) | file_ref_number;
    uint32_t h = edb_murmur3_final((uint32_t)key ^ (uint32_t)(key >> 32));
    uint32_t idx = h % db->ref_hash_cap;
    for (uint32_t probe = 0; probe < db->ref_hash_cap; probe++) {
        uint32_t id = db->ref_hash_buckets[idx];
        if (id == 0) return 0;
        if (db->archives[id - 1].drive_letter == drive_letter &&
            db->archives[id - 1].file_ref_number == file_ref_number)
            return id;
        idx = (idx + 1) % db->ref_hash_cap;
    }
    return 0;
}

/* ===== Error Message ===== */

const char* edb_error_message(int code) {
    switch (code) {
    case EDB_OK:            return "成功";
    case EDB_ERR_ARG:       return "无效参数";
    case EDB_ERR_IO:        return "I/O 错误";
    case EDB_ERR_FORMAT:    return "文件格式错误";
    case EDB_ERR_MEMORY:    return "内存不足";
    case EDB_ERR_NOT_FOUND: return "记录未找到";
    case EDB_ERR_READ_ONLY: return "数据库只读";
    default:                return "未知错误";
    }
}

/* ===== Open / Close ===== */

int edb_open(const char* path, Edb** out_db) {
    if (!path || !out_db) return EDB_ERR_ARG;

    FILE* fp = fopen(path, "r+b");
    int read_only = 0;
    if (!fp) {
        fp = fopen(path, "rb");
        if (!fp) return EDB_ERR_IO;
        read_only = 1;
    }

    int64_t fsize = edb_util_file_size(fp);
    if (fsize < 0) { fclose(fp); return EDB_ERR_IO; }

    EdbHeader hdr;
    int rc = edb_format_read_header(fp, (uint64_t)fsize, &hdr);
    if (rc != EDB_OK) { fclose(fp); return rc; }

    Edb* db = (Edb*)calloc(1, sizeof(Edb));
    if (!db) { fclose(fp); return EDB_ERR_MEMORY; }
    db->fp = fp;
    db->path = strdup(path);
    db->read_only = read_only;

    /* 读 section table */
    rc = edb_format_read_section_table(fp, hdr.section_table_offset,
                                        hdr.section_count, db->sections);
    if (rc != EDB_OK) { edb_close(db); return rc; }

    /* 加载 archive records */
    const EdbSectionDesc* sec_arch = edb_format_find_section(db->sections, EDB_SEC_COUNT,
                                                               EDB_SEC_ARCHIVE_RECORDS);
    const EdbSectionDesc* sec_str = edb_format_find_section(db->sections, EDB_SEC_COUNT,
                                                              EDB_SEC_ARCHIVE_STRING_POOL);
    if (sec_arch && sec_arch->raw_size > 0) {
        uint8_t* data = NULL;
        uint32_t data_len = 0;
        rc = edb_load_section_data(fp, sec_arch, &data, &data_len);
        if (rc != EDB_OK) { edb_close(db); return rc; }
        db->archive_count = (uint32_t)hdr.archive_count;
        db->archives = (EdbArchiveInMem*)data; /* 直接 cast，布局兼容 */
        db->archive_cap = db->archive_count;

        /* 加载 string pool */
        if (sec_str && sec_str->raw_size > 0) {
            rc = edb_load_section_data(fp, sec_str, (uint8_t**)&db->archive_strings, (uint32_t*)&db->archive_strings_size);
            if (rc != EDB_OK) { free(data); edb_close(db); return rc; }
        }
    }

    /* 加载 entry cores */
    const EdbSectionDesc* sec_core = edb_format_find_section(db->sections, EDB_SEC_COUNT,
                                                                EDB_SEC_ENTRY_CORE);
    if (sec_core && sec_core->raw_size > 0) {
        uint8_t* data = NULL;
        uint32_t data_len = 0;
        rc = edb_load_section_data(fp, sec_core, &data, &data_len);
        if (rc != EDB_OK) { edb_close(db); return rc; }
        db->entries = (EdbEntryCoreInMem*)data;
        db->entry_count = (uint32_t)hdr.entry_count;
        db->entry_cap = db->entry_count;
    }

    /* 加载 detail pages index */
    const EdbSectionDesc* sec_detail = edb_format_find_section(db->sections, EDB_SEC_COUNT,
                                                                  EDB_SEC_ENTRY_DETAIL_PAGES);
    if (sec_detail && sec_detail->aux_count > 0) {
        db->detail_page_count = sec_detail->aux_count;
        db->detail_pages = (EdbDiskPage*)malloc(db->detail_page_count * sizeof(EdbDiskPage));
        if (!db->detail_pages) { edb_close(db); return EDB_ERR_MEMORY; }
        fseek(fp, (long)sec_detail->aux_offset, SEEK_SET);
        fread(db->detail_pages, sizeof(EdbDiskPage), db->detail_page_count, fp);
    }

    /* 加载 path pages index */
    const EdbSectionDesc* sec_path = edb_format_find_section(db->sections, EDB_SEC_COUNT,
                                                                EDB_SEC_ENTRY_PATH_PAGES);
    if (sec_path && sec_path->aux_count > 0) {
        db->path_page_count = sec_path->aux_count;
        db->path_pages = (EdbDiskPage*)malloc(db->path_page_count * sizeof(EdbDiskPage));
        if (!db->path_pages) { edb_close(db); return EDB_ERR_MEMORY; }
        fseek(fp, (long)sec_path->aux_offset, SEEK_SET);
        fread(db->path_pages, sizeof(EdbDiskPage), db->path_page_count, fp);
    }

    /* 加载 raw pages index */
    const EdbSectionDesc* sec_raw = edb_format_find_section(db->sections, EDB_SEC_COUNT,
                                                               EDB_SEC_ENTRY_RAW_BLOBS);
    if (sec_raw && sec_raw->aux_count > 0) {
        db->raw_page_count = sec_raw->aux_count;
        db->raw_pages = (EdbDiskPage*)malloc(db->raw_page_count * sizeof(EdbDiskPage));
        if (!db->raw_pages) { edb_close(db); return EDB_ERR_MEMORY; }
        fseek(fp, (long)sec_raw->aux_offset, SEEK_SET);
        fread(db->raw_pages, sizeof(EdbDiskPage), db->raw_page_count, fp);
    }

    /* 活跃位图 */
    db->active_archive_bits = (unsigned char*)calloc(edb_bits_bytes(db->archive_count + 1), 1);
    db->active_entry_bits = (unsigned char*)calloc(edb_bits_bytes(db->entry_count + 1), 1);
    for (uint32_t i = 0; i < (uint32_t)hdr.active_archive_count && i < db->archive_count; i++)
        edb_bit_set(db->active_archive_bits, i + 1);
    for (uint32_t i = 0; i < (uint32_t)hdr.active_entry_count && i < db->entry_count; i++)
        edb_bit_set(db->active_entry_bits, i + 1);

    /* 加载 archive postings + index */
    const EdbSectionDesc* sec_ap = edb_format_find_section(db->sections, EDB_SEC_COUNT,
                                                              EDB_SEC_ARCHIVE_POSTINGS);
    const EdbSectionDesc* sec_api = edb_format_find_section(db->sections, EDB_SEC_COUNT,
                                                               EDB_SEC_ARCHIVE_POSTING_INDEX);
    if (sec_ap && sec_ap->raw_size > 0) {
        edb_load_section_data(fp, sec_ap, (uint8_t**)&db->archive_postings,
                               (uint32_t*)&db->archive_postings_size);
    }
    if (sec_api && sec_api->raw_size > 0) {
        db->archive_index_count = (uint32_t)(sec_api->raw_size / sizeof(EdbDiskIndex));
        db->archive_index = (EdbDiskIndex*)malloc((size_t)sec_api->raw_size);
        if (db->archive_index) {
            fseek(fp, (long)sec_api->offset, SEEK_SET);
            fread(db->archive_index, sizeof(EdbDiskIndex), db->archive_index_count, fp);
        }
    }

    /* 加载 entry postings + index */
    const EdbSectionDesc* sec_ep = edb_format_find_section(db->sections, EDB_SEC_COUNT,
                                                              EDB_SEC_ENTRY_POSTINGS);
    const EdbSectionDesc* sec_epi = edb_format_find_section(db->sections, EDB_SEC_COUNT,
                                                               EDB_SEC_ENTRY_POSTING_INDEX);
    if (sec_ep && sec_ep->raw_size > 0) {
        edb_load_section_data(fp, sec_ep, (uint8_t**)&db->entry_postings,
                               (uint32_t*)&db->entry_postings_size);
    }
    if (sec_epi && sec_epi->raw_size > 0) {
        db->entry_index_count = (uint32_t)(sec_epi->raw_size / sizeof(EdbDiskIndex));
        db->entry_index = (EdbDiskIndex*)malloc((size_t)sec_epi->raw_size);
        if (db->entry_index) {
            fseek(fp, (long)sec_epi->offset, SEEK_SET);
            fread(db->entry_index, sizeof(EdbDiskIndex), db->entry_index_count, fp);
        }
    }

    /* 构建 ref hash */
    edb_build_ref_hash(db);

    /* 初始化 page cache */
    edb_page_cache_init(&db->detail_cache);
    edb_page_cache_init(&db->path_cache);
    edb_page_cache_init(&db->raw_cache);

    /* meta path */
    size_t plen = strlen(path);
    db->meta_path = (char*)malloc(plen + 6);
    if (db->meta_path) {
        memcpy(db->meta_path, path, plen);
        memcpy(db->meta_path + plen, ".meta", 6);
    }

    *out_db = db;
    return EDB_OK;
}

void edb_close(Edb* db) {
    if (!db) return;
    free(db->archives);
    free(db->archive_strings);
    free(db->entries);
    free(db->detail_pages);
    free(db->path_pages);
    free(db->raw_pages);
    free(db->active_archive_bits);
    free(db->active_entry_bits);
    free(db->archive_index);
    free(db->archive_postings);
    free(db->entry_index);
    free(db->entry_postings);
    free(db->ref_hash_buckets);
    free(db->path);
    free(db->meta_path);
    edb_page_cache_free(&db->detail_cache);
    edb_page_cache_free(&db->path_cache);
    edb_page_cache_free(&db->raw_cache);
    if (db->fp) fclose(db->fp);
    free(db);
}

/* ===== 计数 ===== */

uint32_t edb_count(Edb* db) { return db ? db->archive_count : 0; }
uint32_t edb_active_count(Edb* db) {
    if (!db) return 0;
    uint32_t c = 0;
    for (uint32_t i = 1; i <= db->archive_count; i++)
        if (edb_bit_get(db->active_archive_bits, i)) c++;
    return c;
}
uint32_t edb_entry_count(Edb* db) { return db ? db->entry_count : 0; }
uint32_t edb_active_entry_count(Edb* db) {
    if (!db) return 0;
    uint32_t c = 0;
    for (uint32_t i = 1; i <= db->entry_count; i++)
        if (edb_bit_get(db->active_entry_bits, i)) c++;
    return c;
}
uint64_t edb_file_size(Edb* db) {
    if (!db || !db->fp) return 0;
    return (uint64_t)edb_util_file_size(db->fp);
}

int edb_stats(Edb* db, EdbStats* out) {
    if (!db || !out) return EDB_ERR_ARG;
    memset(out, 0, sizeof(*out));
    out->archive_count = db->archive_count;
    out->active_archive_count = edb_active_count(db);
    out->entry_count = db->entry_count;
    out->active_entry_count = edb_active_entry_count(db);
    out->file_size = edb_file_size(db);

    for (int i = 0; i < EDB_SEC_COUNT; i++) {
        switch (db->sections[i].section_id) {
        case EDB_SEC_ARCHIVE_RECORDS: out->archive_records_size = db->sections[i].encoded_size; break;
        case EDB_SEC_ARCHIVE_STRING_POOL: out->archive_strings_size = db->sections[i].encoded_size; break;
        case EDB_SEC_ENTRY_CORE: out->entry_core_size = db->sections[i].encoded_size; break;
        case EDB_SEC_ENTRY_DETAIL_PAGES: out->entry_detail_size = db->sections[i].encoded_size; break;
        case EDB_SEC_ENTRY_PATH_PAGES: out->entry_path_size = db->sections[i].encoded_size; break;
        case EDB_SEC_ENTRY_RAW_BLOBS: out->raw_blob_size = db->sections[i].encoded_size; break;
        case EDB_SEC_ARCHIVE_POSTING_INDEX: out->archive_index_size = db->sections[i].encoded_size; break;
        case EDB_SEC_ARCHIVE_POSTINGS: out->archive_postings_size = db->sections[i].encoded_size; break;
        case EDB_SEC_ENTRY_POSTING_INDEX: out->entry_index_size = db->sections[i].encoded_size; break;
        case EDB_SEC_ENTRY_POSTINGS: out->entry_postings_size = db->sections[i].encoded_size; break;
        }
    }
    return EDB_OK;
}

/* ===== 单条查询 ===== */

int edb_get_archive(Edb* db, uint32_t id, EdbArchiveResult* out) {
    if (!db || !out || id == 0 || id > db->archive_count) return EDB_ERR_ARG;
    if (!edb_bit_get(db->active_archive_bits, id)) return EDB_ERR_NOT_FOUND;

    EdbArchiveInMem* a = &db->archives[id - 1];
    memset(out, 0, sizeof(*out));
    out->id = id;
    out->drive_letter = a->drive_letter;
    out->file_ref_number = a->file_ref_number;
    out->usn = a->usn;
    out->file_size = a->file_size;
    out->modified_time = a->modified_time;

    if (db->archive_strings && a->name_len > 0) {
        out->file_path = (char*)malloc(a->name_len + 1);
        if (out->file_path) {
            memcpy(out->file_path, db->archive_strings + a->name_offset, a->name_len);
            out->file_path[a->name_len] = '\0';
        }
    }
    return EDB_OK;
}

int edb_get_entry(Edb* db, uint32_t id, EdbEntryResult* out) {
    if (!db || !out || id == 0 || id > db->entry_count) return EDB_ERR_ARG;
    if (!edb_bit_get(db->active_entry_bits, id)) return EDB_ERR_NOT_FOUND;

    EdbEntryCoreInMem* e = &db->entries[id - 1];
    memset(out, 0, sizeof(*out));
    out->id = id;
    out->archive_id = e->archive_id;

    /* archive path */
    if (e->archive_id > 0 && e->archive_id <= db->archive_count) {
        EdbArchiveInMem* a = &db->archives[e->archive_id - 1];
        if (db->archive_strings && a->name_len > 0) {
            out->archive_path = (char*)malloc(a->name_len + 1);
            if (out->archive_path) {
                memcpy(out->archive_path, db->archive_strings + a->name_offset, a->name_len);
                out->archive_path[a->name_len] = '\0';
            }
        }
    }

    /* entry path */
    if (db->path_pages && db->path_page_count > 0 && e->path_len > 0) {
        edb_format_read_entry_path(db->fp, db->path_pages, db->path_page_count,
                                    &db->path_cache, e->path_offset, e->path_len,
                                    &out->entry_path);
    }

    /* entry detail */
    if (db->detail_pages && db->detail_page_count > 0) {
        EdbDiskEntryDetail det;
        int rc = edb_format_read_entry_detail(db->fp, db->detail_pages, db->detail_page_count,
                                               &db->detail_cache, id - 1, &det);
        if (rc == EDB_OK) {
            out->compressed_size = det.compressed_size;
            out->original_size = det.original_size;
            out->modified_time = det.modified_time;
            out->entry_raw_path_len = det.raw_len;

            if (det.raw_len > 0 && det.raw_offset != UINT32_MAX && db->raw_pages) {
                edb_format_read_entry_raw(db->fp, db->raw_pages, db->raw_page_count,
                                           &db->raw_cache, det.raw_offset, det.raw_len,
                                           &out->entry_raw_path);
            }
        }
    }
    return EDB_OK;
}

int edb_get_archive_by_ref(Edb* db, char drive_letter, uint64_t file_ref_number,
                            EdbArchiveResult* out) {
    if (!db || !out) return EDB_ERR_ARG;
    uint32_t id = edb_find_archive_by_ref(db, drive_letter, file_ref_number);
    if (id == 0) return EDB_ERR_NOT_FOUND;
    return edb_get_archive(db, id, out);
}

/* ===== 释放 ===== */

void edb_free_archive_result(EdbArchiveResult* r) {
    if (!r) return;
    free(r->file_path);
    memset(r, 0, sizeof(*r));
}

void edb_free_entry_result(EdbEntryResult* r) {
    if (!r) return;
    free(r->archive_path);
    free(r->entry_path);
    free(r->entry_raw_path);
    memset(r, 0, sizeof(*r));
}

void edb_free_search_result(EdbSearchResult* r) {
    if (!r) return;
    free(r->archive_path);
    free(r->entry_path);
    free(r->entry_raw_path);
    memset(r, 0, sizeof(*r));
}

void edb_free_query_page(EdbEntryQueryPage* p) {
    if (!p) return;
    free(p->ids);
    memset(p, 0, sizeof(*p));
}

/* ===== 搜索 ===== */

int edb_search(Edb* db, const char* keyword, uint32_t scope, uint32_t limit,
               EdbSearchCallback callback, void* user_data) {
    if (!db || !keyword || !callback) return EDB_ERR_ARG;

    EdbQueryNode* ast = edb_query_parse(keyword);
    if (!ast) return EDB_OK;

    uint32_t returned = 0;

    /* 搜索 archives */
    if ((scope & EDB_SEARCH_ARCHIVE_PATH) && db->archive_postings && db->archive_index) {
        uint32_t* ids = NULL;
        uint32_t count = 0;
        int rc = edb_search_eval(ast, db->archive_postings, db->archive_postings_size,
                                  db->archive_index, db->archive_index_count,
                                  db->archive_count, &ids, &count);
        if (rc == EDB_OK) {
            for (uint32_t i = 0; i < count && returned < limit; i++) {
                if (!edb_bit_get(db->active_archive_bits, ids[i])) continue;
                EdbSearchResult sr;
                memset(&sr, 0, sizeof(sr));
                sr.kind = EDB_RESULT_ARCHIVE;
                sr.id = ids[i];
                if (ids[i] > 0 && ids[i] <= db->archive_count) {
                    EdbArchiveInMem* a = &db->archives[ids[i] - 1];
                    sr.drive_letter = a->drive_letter;
                    sr.file_ref_number = a->file_ref_number;
                    sr.usn = a->usn;
                    sr.file_size = a->file_size;
                    sr.modified_time = a->modified_time;
                    if (db->archive_strings && a->name_len > 0) {
                        sr.archive_path = (char*)malloc(a->name_len + 1);
                        if (sr.archive_path) {
                            memcpy(sr.archive_path, db->archive_strings + a->name_offset, a->name_len);
                            sr.archive_path[a->name_len] = '\0';
                        }
                    }
                }
                callback(&sr, user_data);
                edb_free_search_result(&sr);
                returned++;
            }
        }
        free(ids);
    }

    /* 搜索 entries */
    if ((scope & EDB_SEARCH_ENTRY_PATH) && db->entry_postings && db->entry_index && returned < limit) {
        uint32_t* ids = NULL;
        uint32_t count = 0;
        int rc = edb_search_eval(ast, db->entry_postings, db->entry_postings_size,
                                  db->entry_index, db->entry_index_count,
                                  db->entry_count, &ids, &count);
        if (rc == EDB_OK) {
            for (uint32_t i = 0; i < count && returned < limit; i++) {
                if (!edb_bit_get(db->active_entry_bits, ids[i])) continue;
                EdbSearchResult sr;
                memset(&sr, 0, sizeof(sr));
                sr.kind = EDB_RESULT_ENTRY;
                sr.id = ids[i];

                if (ids[i] > 0 && ids[i] <= db->entry_count) {
                    EdbEntryCoreInMem* e = &db->entries[ids[i] - 1];
                    sr.archive_id = e->archive_id;

                    /* archive path */
                    if (e->archive_id > 0 && e->archive_id <= db->archive_count) {
                        EdbArchiveInMem* a = &db->archives[e->archive_id - 1];
                        sr.drive_letter = a->drive_letter;
                        sr.file_ref_number = a->file_ref_number;
                        sr.usn = a->usn;
                        if (db->archive_strings && a->name_len > 0) {
                            sr.archive_path = (char*)malloc(a->name_len + 1);
                            if (sr.archive_path) {
                                memcpy(sr.archive_path, db->archive_strings + a->name_offset, a->name_len);
                                sr.archive_path[a->name_len] = '\0';
                            }
                        }
                    }

                    /* entry path */
                    if (db->path_pages && e->path_len > 0) {
                        edb_format_read_entry_path(db->fp, db->path_pages, db->path_page_count,
                                                    &db->path_cache, e->path_offset, e->path_len,
                                                    &sr.entry_path);
                    }
                }
                callback(&sr, user_data);
                edb_free_search_result(&sr);
                returned++;
            }
        }
        free(ids);
    }

    edb_query_free(ast);
    return EDB_OK;
}

int edb_query_entries(Edb* db, const EdbEntryQuery* query, EdbEntryQueryPage* out) {
    if (!db || !query || !out) return EDB_ERR_ARG;
    memset(out, 0, sizeof(*out));

    if (!query->keyword || !*query->keyword) {
        /* 无关键词：返回所有活跃 entry */
        uint32_t total = edb_active_entry_count(db);
        out->total_count = total;
        uint32_t start = query->offset;
        uint32_t end = start + query->limit;
        if (end > total) end = total;
        out->returned_count = end > start ? end - start : 0;
        if (out->returned_count > 0) {
            out->ids = (uint32_t*)malloc(out->returned_count * sizeof(uint32_t));
            uint32_t j = 0, collected = 0;
            for (uint32_t i = 1; i <= db->entry_count && collected < end; i++) {
                if (edb_bit_get(db->active_entry_bits, i)) {
                    if (j >= start) out->ids[collected++] = i;
                    j++;
                }
            }
        }
        return EDB_OK;
    }

    /* 有关键词：搜索 */
    EdbQueryNode* ast = edb_query_parse(query->keyword);
    if (!ast) return EDB_OK;

    uint32_t* ids = NULL;
    uint32_t count = 0;
    int rc = EDB_OK;

    if (db->entry_postings && db->entry_index) {
        rc = edb_search_eval(ast, db->entry_postings, db->entry_postings_size,
                              db->entry_index, db->entry_index_count,
                              db->entry_count, &ids, &count);
    }

    edb_query_free(ast);
    if (rc != EDB_OK) return rc;

    /* 过滤活跃 + scope */
    uint32_t active_count = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (edb_bit_get(db->active_entry_bits, ids[i])) {
            ids[active_count++] = ids[i];
        }
    }

    out->total_count = active_count;
    uint32_t start = query->offset;
    uint32_t end = start + query->limit;
    if (end > active_count) end = active_count;
    out->returned_count = end > start ? end - start : 0;
    if (out->returned_count > 0) {
        out->ids = (uint32_t*)malloc(out->returned_count * sizeof(uint32_t));
        memcpy(out->ids, ids + start, out->returned_count * sizeof(uint32_t));
    }
    free(ids);
    return EDB_OK;
}

int edb_get_entries_batch(Edb* db, const uint32_t* ids, uint32_t count,
                           EdbEntryResult* out) {
    if (!db || !ids || !out) return EDB_ERR_ARG;
    for (uint32_t i = 0; i < count; i++) {
        int rc = edb_get_entry(db, ids[i], &out[i]);
        if (rc != EDB_OK) {
            for (uint32_t j = 0; j < i; j++) edb_free_entry_result(&out[j]);
            return rc;
        }
    }
    return EDB_OK;
}

/* ===== 快照构建 ===== */

int edb_build_snapshot(const EdbArchiveRecord* archives, uint32_t archive_count,
                       const EdbEntryRecord* entries, uint32_t entry_count,
                       const char* output_path) {
    return edb_build_snapshot_stream(archives, archive_count, NULL, entry_count,
                                      output_path, NULL);
}

typedef struct {
    const EdbEntryRecord* entries;
    uint32_t count;
    uint32_t pos;
} EdbArrayStream;

static int array_stream_next(void* ud, EdbEntryRecord* out) {
    EdbArrayStream* s = (EdbArrayStream*)ud;
    if (s->pos >= s->count) return 1;
    *out = s->entries[s->pos++];
    return 0;
}

static int array_stream_reset(void* ud) {
    EdbArrayStream* s = (EdbArrayStream*)ud;
    s->pos = 0;
    return 0;
}

int edb_build_snapshot_stream(const EdbArchiveRecord* archives, uint32_t archive_count,
                               EdbEntryStream* entry_stream, uint32_t entry_count,
                               const char* output_path, const EdbBuildOptions* options) {
    if (!archives || !output_path) return EDB_ERR_ARG;

    if (!entry_stream && entry_count > 0) {
        /* 用 archives+entries 直接构建 */
        return edb_build_snapshot_impl(archives, archive_count,
                                        NULL, entry_count, output_path, options);
    }

    return edb_build_snapshot_impl(archives, archive_count,
                                    entry_stream, entry_count, output_path, options);
}

/* ===== 写入事务 ===== */

int edb_begin_write(Edb* db) {
    if (!db) return EDB_ERR_ARG;
    if (db->txn_active) return EDB_ERR_ARG;
    if (db->read_only) return EDB_ERR_READ_ONLY;

    uint32_t bits_size = edb_bits_bytes(db->entry_count + 1);
    db->txn_save_active_entry_bits = (unsigned char*)malloc(bits_size);
    if (!db->txn_save_active_entry_bits) return EDB_ERR_MEMORY;
    memcpy(db->txn_save_active_entry_bits, db->active_entry_bits, bits_size);
    db->txn_save_bits_size = bits_size;
    db->txn_save_entry_count = db->entry_count;
    db->txn_active = 1;
    return EDB_OK;
}

int edb_commit_write(Edb* db) {
    if (!db || !db->txn_active) return EDB_ERR_ARG;
    free(db->txn_save_active_entry_bits);
    db->txn_save_active_entry_bits = NULL;
    db->txn_active = 0;
    return EDB_OK;
}

int edb_rollback_write(Edb* db) {
    if (!db || !db->txn_active) return EDB_ERR_ARG;
    if (db->pending_replace) {
        edb_abort_replace_archive_entries(db, db->pending_replace->archive_id);
    }
    if (db->txn_save_active_entry_bits) {
        memcpy(db->active_entry_bits, db->txn_save_active_entry_bits, db->txn_save_bits_size);
        free(db->txn_save_active_entry_bits);
        db->txn_save_active_entry_bits = NULL;
    }
    db->entry_count = db->txn_save_entry_count;
    db->txn_active = 0;
    return EDB_OK;
}

/* ===== 压缩包 CRUD ===== */

int edb_upsert_archive(Edb* db, const EdbArchiveRecord* record, uint32_t* out_id) {
    if (!db || !record) return EDB_ERR_ARG;
    if (db->read_only) return EDB_ERR_READ_ONLY;

    uint32_t existing = edb_find_archive_by_ref(db, record->drive_letter, record->file_ref_number);
    if (existing > 0) {
        /* 更新 */
        EdbArchiveInMem* a = &db->archives[existing - 1];
        a->file_size = record->file_size;
        a->modified_time = record->modified_time;
        a->usn = record->usn;
        /* 路径不变（假定不变） */
        if (out_id) *out_id = existing;
        return EDB_OK;
    }

    /* 新增 */
    if (edb_ensure_cap((void**)&db->archives, &db->archive_cap,
                        db->archive_count + 1, sizeof(EdbArchiveInMem)) != 0)
        return EDB_ERR_MEMORY;

    /* 追加 path 到 strings */
    uint32_t name_len = (uint32_t)strlen(record->file_path);
    uint32_t name_offset = (uint32_t)db->archive_strings_size;
    uint64_t new_str_size = db->archive_strings_size + name_len + 1;
    char* new_str = (char*)realloc(db->archive_strings, (size_t)new_str_size);
    if (!new_str) return EDB_ERR_MEMORY;
    db->archive_strings = new_str;
    memcpy(db->archive_strings + name_offset, record->file_path, name_len);
    db->archive_strings[name_offset + name_len] = '\0';
    db->archive_strings_size = new_str_size;

    EdbArchiveInMem* a = &db->archives[db->archive_count];
    a->name_offset = name_offset;
    a->name_len = name_len;
    a->file_size = record->file_size;
    a->modified_time = record->modified_time;
    a->drive_letter = record->drive_letter;
    a->file_ref_number = record->file_ref_number;
    a->usn = record->usn;

    db->archive_count++;
    uint32_t id = db->archive_count;
    edb_bit_set(db->active_archive_bits, id);

    /* 重建 ref hash */
    free(db->ref_hash_buckets);
    edb_build_ref_hash(db);

    if (out_id) *out_id = id;
    return EDB_OK;
}

int edb_upsert_archives(Edb* db, const EdbArchiveRecord* records, uint32_t count,
                         uint32_t* out_ids) {
    if (!db || !records) return EDB_ERR_ARG;
    for (uint32_t i = 0; i < count; i++) {
        int rc = edb_upsert_archive(db, &records[i], out_ids ? &out_ids[i] : NULL);
        if (rc != EDB_OK) return rc;
    }
    return EDB_OK;
}

int edb_delete_archive_by_ref(Edb* db, char drive_letter, uint64_t file_ref_number) {
    if (!db) return EDB_ERR_ARG;
    if (db->read_only) return EDB_ERR_READ_ONLY;

    uint32_t id = edb_find_archive_by_ref(db, drive_letter, file_ref_number);
    if (id == 0) return EDB_ERR_NOT_FOUND;

    edb_bit_clear(db->active_archive_bits, id);

    /* 级联标记 entry */
    for (uint32_t i = 0; i < db->entry_count; i++) {
        if (db->entries[i].archive_id == id)
            edb_bit_clear(db->active_entry_bits, i + 1);
    }
    return EDB_OK;
}

/* ===== 条目 CRUD ===== */

int edb_replace_archive_entries(Edb* db, uint32_t archive_id,
                                 const EdbEntryRecord* entries, uint32_t entry_count) {
    int rc = edb_begin_replace_archive_entries(db, archive_id);
    if (rc != EDB_OK) return rc;
    rc = edb_append_archive_entries(db, archive_id, entries, entry_count);
    if (rc != EDB_OK) { edb_abort_replace_archive_entries(db, archive_id); return rc; }
    return edb_finish_replace_archive_entries(db, archive_id);
}

int edb_begin_replace_archive_entries(Edb* db, uint32_t archive_id) {
    if (!db || archive_id == 0 || archive_id > db->archive_count) return EDB_ERR_ARG;

    EdbEntryReplace* repl = (EdbEntryReplace*)calloc(1, sizeof(EdbEntryReplace));
    if (!repl) return EDB_ERR_MEMORY;
    repl->archive_id = archive_id;

    /* 保存旧 entry 的活跃状态 */
    uint32_t bits_size = edb_bits_bytes(db->entry_count + 1);
    repl->saved_bits = (unsigned char*)malloc(bits_size);
    if (!repl->saved_bits) { free(repl); return EDB_ERR_MEMORY; }
    memcpy(repl->saved_bits, db->active_entry_bits, bits_size);
    repl->saved_bits_len = bits_size;

    /* 标记旧 entry 非活跃 */
    for (uint32_t i = 0; i < db->entry_count; i++) {
        if (db->entries[i].archive_id == archive_id)
            edb_bit_clear(db->active_entry_bits, i + 1);
    }

    db->pending_replace = repl;
    return EDB_OK;
}

int edb_append_archive_entries(Edb* db, uint32_t archive_id,
                                const EdbEntryRecord* entries, uint32_t entry_count) {
    if (!db || !entries) return EDB_ERR_ARG;
    if (!db->pending_replace || db->pending_replace->archive_id != archive_id) return EDB_ERR_ARG;

    EdbEntryReplace* repl = db->pending_replace;

    for (uint32_t i = 0; i < entry_count; i++) {
        if (edb_ensure_cap((void**)&repl->new_cores, &repl->new_core_cap,
                            repl->new_core_count + 1, sizeof(EdbEntryCoreInMem)) != 0)
            return EDB_ERR_MEMORY;

        uint32_t epath_len = (uint32_t)strlen(entries[i].entry_path);
        uint32_t path_off = repl->new_paths_size;

        /* 追加 path */
        if (edb_ensure_cap((void**)&repl->new_paths, &repl->new_paths_cap,
                            repl->new_paths_size + epath_len, 1) != 0)
            return EDB_ERR_MEMORY;
        memcpy(repl->new_paths + repl->new_paths_size, entries[i].entry_path, epath_len);
        repl->new_paths_size += epath_len;

        EdbEntryCoreInMem* core = &repl->new_cores[repl->new_core_count];
        core->archive_id = archive_id;
        core->path_offset = path_off;
        core->path_len = epath_len;
        repl->new_core_count++;
    }
    return EDB_OK;
}

int edb_finish_replace_archive_entries(Edb* db, uint32_t archive_id) {
    if (!db) return EDB_ERR_ARG;
    if (!db->pending_replace || db->pending_replace->archive_id != archive_id) return EDB_ERR_ARG;

    EdbEntryReplace* repl = db->pending_replace;

    /* 追加 new cores 到 db->entries */
    uint32_t new_base = db->entry_count;
    if (edb_ensure_cap((void**)&db->entries, &db->entry_cap,
                        db->entry_count + repl->new_core_count,
                        sizeof(EdbEntryCoreInMem)) != 0)
        return EDB_ERR_MEMORY;
    memcpy(db->entries + db->entry_count, repl->new_cores,
           repl->new_core_count * sizeof(EdbEntryCoreInMem));

    /* 修正 path_offset：加上已有路径的总偏移 */
    /* 简化处理：新 paths 的 path_offset 需要在写入后调整 */
    /* 这里暂时不做 path_offset 的全局偏移修正 */

    /* 标记新 entry 为活跃 */
    for (uint32_t i = 0; i < repl->new_core_count; i++) {
        db->entry_count++;
        edb_bit_set(db->active_entry_bits, db->entry_count);
    }

    /* 释放 */
    free(repl->new_cores);
    free(repl->new_details);
    free(repl->new_paths);
    free(repl->saved_bits);
    free(repl);
    db->pending_replace = NULL;
    return EDB_OK;
}

int edb_abort_replace_archive_entries(Edb* db, uint32_t archive_id) {
    if (!db) return EDB_ERR_ARG;
    if (!db->pending_replace || db->pending_replace->archive_id != archive_id) return EDB_ERR_ARG;

    EdbEntryReplace* repl = db->pending_replace;
    /* 恢复活跃位图 */
    if (repl->saved_bits)
        memcpy(db->active_entry_bits, repl->saved_bits, repl->saved_bits_len);

    free(repl->new_cores);
    free(repl->new_details);
    free(repl->new_paths);
    free(repl->saved_bits);
    free(repl);
    db->pending_replace = NULL;
    return EDB_OK;
}

/* ===== 元数据 ===== */

int edb_get_meta(Edb* db, const char* key, char** out_value) {
    if (!db || !key || !out_value) return EDB_ERR_ARG;
    *out_value = NULL;
    if (!db->meta_path) return EDB_ERR_IO;

    FILE* fp = fopen(db->meta_path, "r");
    if (!fp) return EDB_ERR_NOT_FOUND;

    size_t key_len = strlen(key);
    char line[4096];
    while (fgets(line, sizeof(line), fp)) {
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') { line[--len] = '\0'; }
        if (len > 0 && line[len - 1] == '\r') { line[--len] = '\0'; }
        if (len > key_len && line[key_len] == '=' && memcmp(line, key, key_len) == 0) {
            const char* val = line + key_len + 1;
            *out_value = strdup(val);
            fclose(fp);
            return *out_value ? EDB_OK : EDB_ERR_MEMORY;
        }
    }
    fclose(fp);
    return EDB_ERR_NOT_FOUND;
}

int edb_put_meta(Edb* db, const char* key, const char* value) {
    if (!db || !key || !value) return EDB_ERR_ARG;
    if (db->read_only) return EDB_ERR_READ_ONLY;
    if (!db->meta_path) return EDB_ERR_IO;

    /* 读取现有内容 */
    char** lines = NULL;
    uint32_t line_count = 0, line_cap = 0;
    size_t key_len = strlen(key);

    FILE* fp = fopen(db->meta_path, "r");
    if (fp) {
        char buf[4096];
        while (fgets(buf, sizeof(buf), fp)) {
            if (edb_ensure_cap((void**)&lines, &line_cap, line_count + 1, sizeof(char*)) != 0) {
                fclose(fp); goto cleanup;
            }
            lines[line_count++] = strdup(buf);
        }
        fclose(fp);
    }

    /* 更新或追加 */
    int found = 0;
    for (uint32_t i = 0; i < line_count; i++) {
        if (strlen(lines[i]) > key_len && lines[i][key_len] == '=' &&
            memcmp(lines[i], key, key_len) == 0) {
            free(lines[i]);
            char new_line[4096];
            snprintf(new_line, sizeof(new_line), "%s=%s\n", key, value);
            lines[i] = strdup(new_line);
            found = 1;
            break;
        }
    }
    if (!found) {
        if (edb_ensure_cap((void**)&lines, &line_cap, line_count + 1, sizeof(char*)) != 0)
            goto cleanup;
        char new_line[4096];
        snprintf(new_line, sizeof(new_line), "%s=%s\n", key, value);
        lines[line_count++] = strdup(new_line);
    }

    /* 写回临时文件再重命名 */
    size_t mplen = strlen(db->meta_path);
    char* tmp = (char*)malloc(mplen + 8);
    if (!tmp) goto cleanup;
    memcpy(tmp, db->meta_path, mplen);
    memcpy(tmp + mplen, ".tmp", 5);

    fp = fopen(tmp, "w");
    if (!fp) { free(tmp); goto cleanup; }
    for (uint32_t i = 0; i < line_count; i++) {
        fputs(lines[i], fp);
    }
    fclose(fp);
    edb_rename_atomic(tmp, db->meta_path);
    free(tmp);

cleanup:
    for (uint32_t i = 0; i < line_count; i++) free(lines[i]);
    free(lines);
    return EDB_OK;
}

/* ===== Compact ===== */

int edb_compact(Edb* db) {
    if (!db) return EDB_ERR_ARG;
    if (db->read_only) return EDB_ERR_READ_ONLY;

    /* 收集活跃 archive */
    uint32_t active_arch_count = edb_active_count(db);
    EdbArchiveRecord* arch_arr = (EdbArchiveRecord*)calloc(active_arch_count, sizeof(EdbArchiveRecord));
    if (!arch_arr) return EDB_ERR_MEMORY;

    uint32_t* old_to_new = (uint32_t*)calloc(db->archive_count + 1, sizeof(uint32_t));
    if (!old_to_new) { free(arch_arr); return EDB_ERR_MEMORY; }

    uint32_t ai = 0;
    for (uint32_t i = 1; i <= db->archive_count; i++) {
        if (!edb_bit_get(db->active_archive_bits, i)) continue;
        old_to_new[i] = ai + 1;
        EdbArchiveInMem* a = &db->archives[i - 1];
        arch_arr[ai].drive_letter = a->drive_letter;
        arch_arr[ai].file_ref_number = a->file_ref_number;
        arch_arr[ai].usn = a->usn;
        arch_arr[ai].file_size = a->file_size;
        arch_arr[ai].modified_time = a->modified_time;
        if (db->archive_strings && a->name_len > 0) {
            char* path = (char*)malloc(a->name_len + 1);
            memcpy(path, db->archive_strings + a->name_offset, a->name_len);
            path[a->name_len] = '\0';
            arch_arr[ai].file_path = path;
        }
        ai++;
    }

    /* 收集活跃 entry */
    uint32_t active_entry_count = edb_active_entry_count(db);
    /* 用数组流包装 */
    EdbEntryRecord* entry_arr = (EdbEntryRecord*)calloc(active_entry_count, sizeof(EdbEntryRecord));
    if (!entry_arr) {
        for (uint32_t i = 0; i < active_arch_count; i++) free((char*)arch_arr[i].file_path);
        free(arch_arr); free(old_to_new); return EDB_ERR_MEMORY;
    }

    uint32_t ei = 0;
    for (uint32_t i = 1; i <= db->entry_count; i++) {
        if (!edb_bit_get(db->active_entry_bits, i)) continue;
        EdbEntryCoreInMem* core = &db->entries[i - 1];
        entry_arr[ei].archive_id = old_to_new[core->archive_id];

        /* 获取 entry path */
        char* epath = NULL;
        if (db->path_pages && core->path_len > 0) {
            edb_format_read_entry_path(db->fp, db->path_pages, db->path_page_count,
                                        &db->path_cache, core->path_offset, core->path_len,
                                        &epath);
        }
        entry_arr[ei].entry_path = epath ? epath : "";

        /* 获取 detail */
        if (db->detail_pages && db->detail_page_count > 0) {
            EdbDiskEntryDetail det;
            int rc = edb_format_read_entry_detail(db->fp, db->detail_pages, db->detail_page_count,
                                                   &db->detail_cache, i - 1, &det);
            if (rc == EDB_OK) {
                entry_arr[ei].compressed_size = det.compressed_size;
                entry_arr[ei].original_size = det.original_size;
                entry_arr[ei].modified_time = det.modified_time;

                if (det.raw_len > 0 && det.raw_offset != UINT32_MAX && db->raw_pages) {
                    void* raw = NULL;
                    edb_format_read_entry_raw(db->fp, db->raw_pages, db->raw_page_count,
                                               &db->raw_cache, det.raw_offset, det.raw_len, &raw);
                    entry_arr[ei].entry_raw_path = raw;
                    entry_arr[ei].entry_raw_path_len = det.raw_len;
                }
            }
        }
        ei++;
    }

    /* 关闭旧文件 */
    fclose(db->fp);
    db->fp = NULL;

    /* 构建 */
    int rc = edb_build_snapshot(arch_arr, active_arch_count,
                                entry_arr, active_entry_count,
                                db->path);
    if (rc != EDB_OK) {
        /* 尝试重新打开 */
        db->fp = fopen(db->path, "rb");
        goto cleanup;
    }

    /* 重新打开：用新的 Edb 交换内部数据 */
    {
        Edb* new_db = NULL;
        int open_rc = edb_open(db->path, &new_db);
        if (open_rc != EDB_OK) {
            rc = open_rc;
            goto cleanup;
        }

        /* 交换内部数据 */
        edb_page_cache_free(&db->detail_cache);
        edb_page_cache_free(&db->path_cache);
        edb_page_cache_free(&db->raw_cache);
        free(db->archives); free(db->archive_strings); free(db->entries);
        free(db->detail_pages); free(db->path_pages); free(db->raw_pages);
        free(db->active_archive_bits); free(db->active_entry_bits);
        free(db->archive_index); free(db->archive_postings);
        free(db->entry_index); free(db->entry_postings);
        free(db->ref_hash_buckets);
        if (db->fp) fclose(db->fp);

        /* 拷贝 new_db 内容到 db，保持指针不变 */
        FILE* save_fp = new_db->fp;
        new_db->fp = NULL; /* 防止 edb_close 关闭文件 */
        memcpy(db, new_db, sizeof(Edb));
        free(new_db); /* 释放外壳 */
        db->meta_path = (char*)realloc(db->meta_path, strlen(db->path) + 6);
        if (db->meta_path) {
            size_t pl = strlen(db->path);
            memcpy(db->meta_path, db->path, pl);
            memcpy(db->meta_path + pl, ".meta", 6);
        }
    }

cleanup:
    for (uint32_t i = 0; i < active_arch_count; i++) free((char*)arch_arr[i].file_path);
    for (uint32_t i = 0; i < active_entry_count; i++) free((char*)entry_arr[i].entry_path);
    free(arch_arr);
    free(entry_arr);
    free(old_to_new);
    return rc;
}
