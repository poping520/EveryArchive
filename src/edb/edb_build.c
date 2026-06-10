#include "edb_build.h"
#include "edb_util.h"
#include <stdlib.h>
#include <string.h>

/* ===== 分页写入器 ===== */

int edb_paged_writer_init(EdbPagedWriter* w, FILE* fp, uint32_t page_size) {
    memset(w, 0, sizeof(*w));
    w->fp = fp;
    w->page_size = page_size;
    w->buf = (uint8_t*)malloc(page_size);
    if (!w->buf) return EDB_ERR_MEMORY;
    return EDB_OK;
}

void edb_paged_writer_free(EdbPagedWriter* w) {
    free(w->buf);
    free(w->pages);
    memset(w, 0, sizeof(*w));
}

static int edb_paged_writer_flush_page(EdbPagedWriter* w) {
    if (w->buf_used == 0) return EDB_OK;

    /* 记录页信息 */
    if (edb_ensure_cap((void**)&w->pages, &w->page_cap, w->page_count + 1,
                        sizeof(EdbDiskPage)) != 0)
        return EDB_ERR_MEMORY;

    EdbDiskPage* pg = &w->pages[w->page_count];
    pg->offset = (uint64_t)ftell(w->fp);
    pg->raw_size = w->buf_used;

    /* 尝试压缩 */
    uint8_t* comp = NULL;
    uint32_t comp_size = 0;
    int rc = edb_format_compress(w->buf, w->buf_used, &comp, &comp_size);

    if (rc == 0 && comp) {
        pg->encoded_size = comp_size;
        pg->flags = EDB_SECTION_COMPRESSED;
        if (fwrite(comp, 1, comp_size, w->fp) != comp_size) { free(comp); return EDB_ERR_IO; }
        free(comp);
    } else {
        pg->encoded_size = w->buf_used;
        pg->flags = 0;
        if (fwrite(w->buf, 1, w->buf_used, w->fp) != w->buf_used) return EDB_ERR_IO;
    }

    w->page_count++;
    w->buf_used = 0;
    return EDB_OK;
}

int edb_paged_writer_append(EdbPagedWriter* w, const void* data, uint32_t len) {
    const uint8_t* p = (const uint8_t*)data;
    uint32_t remain = len;
    while (remain > 0) {
        uint32_t avail = w->page_size - w->buf_used;
        uint32_t chunk = remain < avail ? remain : avail;
        memcpy(w->buf + w->buf_used, p, chunk);
        w->buf_used += chunk;
        p += chunk;
        remain -= chunk;
        if (w->buf_used >= w->page_size) {
            int rc = edb_paged_writer_flush_page(w);
            if (rc != EDB_OK) return rc;
        }
    }
    return EDB_OK;
}

int edb_paged_writer_flush(EdbPagedWriter* w) {
    return edb_paged_writer_flush_page(w);
}

uint64_t edb_paged_writer_total_raw(EdbPagedWriter* w) {
    uint64_t total = 0;
    for (uint32_t i = 0; i < w->page_count; i++)
        total += w->pages[i].raw_size;
    total += w->buf_used;
    return total;
}

/* ===== 字符串去重器 ===== */

int edb_string_dedup_init(EdbStringDedup* d, uint32_t bucket_count, uint32_t pool_cap) {
    memset(d, 0, sizeof(*d));
    d->pool = (char*)malloc(pool_cap);
    if (!d->pool) return EDB_ERR_MEMORY;
    d->pool_size = 0;
    d->pool_cap = pool_cap;
    d->buckets = (uint32_t*)calloc(bucket_count, sizeof(uint32_t));
    if (!d->buckets) { free(d->pool); return EDB_ERR_MEMORY; }
    d->bucket_count = bucket_count;
    return EDB_OK;
}

void edb_string_dedup_free(EdbStringDedup* d) {
    free(d->pool);
    free(d->buckets);
    memset(d, 0, sizeof(*d));
}

int edb_string_dedup_intern(EdbStringDedup* d, const char* str, uint32_t len,
                             uint32_t* out_offset) {
    uint32_t h = edb_fnv1a(str, len);
    uint32_t idx = edb_murmur3_final(h) % d->bucket_count;

    /* 查找 */
    uint32_t off = d->buckets[idx];
    while (off != 0) {
        if (off - 1 + len <= d->pool_size) {
            if (memcmp(d->pool + off - 1, str, len) == 0) {
                *out_offset = off - 1;
                return EDB_OK;
            }
        }
        /* 简单线性探测 */
        idx = (idx + 1) % d->bucket_count;
        off = d->buckets[idx];
    }

    /* 未找到，插入 */
    if (d->pool_size + len + 1 > d->pool_cap) {
        uint32_t new_cap = d->pool_cap * 2;
        while (new_cap < d->pool_size + len + 1) new_cap *= 2;
        char* p = (char*)realloc(d->pool, new_cap);
        if (!p) return EDB_ERR_MEMORY;
        d->pool = p;
        d->pool_cap = new_cap;
    }

    *out_offset = d->pool_size;
    memcpy(d->pool + d->pool_size, str, len);
    d->pool[d->pool_size + len] = '\0';
    d->pool_size += len + 1;

    /* 在哈希表中找一个空桶 */
    uint32_t insert_idx = edb_murmur3_final(h) % d->bucket_count;
    while (d->buckets[insert_idx] != 0) {
        insert_idx = (insert_idx + 1) % d->bucket_count;
    }
    d->buckets[insert_idx] = *out_offset + 1; /* 1-based */
    return EDB_OK;
}

/* ===== 快照构建 ===== */

static int edb_write_disk_archive(FILE* fp, const EdbArchiveRecord* rec,
                                   uint32_t name_offset, uint32_t name_len) {
    EdbDiskArchive da;
    memset(&da, 0, sizeof(da));
    da.name_offset = name_offset;
    da.name_len = name_len;
    da.file_size = rec->file_size;
    da.modified_time = rec->modified_time;
    da.drive_letter = rec->drive_letter;
    da.file_ref_number = rec->file_ref_number;
    da.usn = rec->usn;
    return fwrite(&da, sizeof(da), 1, fp) == 1 ? EDB_OK : EDB_ERR_IO;
}

static int edb_write_section_data(FILE* fp, const uint8_t* data, uint32_t len,
                                   EdbSectionDesc* sec) {
    sec->offset = (uint64_t)ftell(fp);
    sec->raw_size = len;

    uint8_t* comp = NULL;
    uint32_t comp_size = 0;
    int rc = edb_format_compress(data, len, &comp, &comp_size);

    if (rc == 0 && comp) {
        sec->encoded_size = comp_size;
        sec->flags = EDB_SECTION_COMPRESSED;
        if (fwrite(comp, 1, comp_size, fp) != comp_size) { free(comp); return EDB_ERR_IO; }
        free(comp);
    } else {
        sec->encoded_size = len;
        sec->flags = 0;
        if (fwrite(data, 1, len, fp) != len) return EDB_ERR_IO;
    }
    return EDB_OK;
}

int edb_build_snapshot_impl(const EdbArchiveRecord* archives, uint32_t archive_count,
                             EdbEntryStream* entry_stream, uint32_t entry_count,
                             const char* output_path, const EdbBuildOptions* options) {
    int rc;

    /* 创建临时文件 */
    char* tmp_path = NULL;
    if (options && options->temp_dir && options->temp_dir[0]) {
        const char* sep = strrchr(output_path, '/');
        if (!sep) sep = strrchr(output_path, '\\');
        const char* base = sep ? sep + 1 : output_path;
        size_t dlen = strlen(options->temp_dir);
        size_t blen = strlen(base);
        tmp_path = (char*)malloc(dlen + 1 + blen + 8);
        if (!tmp_path) return EDB_ERR_MEMORY;
        memcpy(tmp_path, options->temp_dir, dlen);
        tmp_path[dlen] = '/';
        memcpy(tmp_path + dlen + 1, base, blen);
        memcpy(tmp_path + dlen + 1 + blen, ".tmp", 5);
    } else {
        size_t pathlen = strlen(output_path);
        tmp_path = (char*)malloc(pathlen + 8);
        if (!tmp_path) return EDB_ERR_MEMORY;
        memcpy(tmp_path, output_path, pathlen);
        memcpy(tmp_path + pathlen, ".tmp", 5);
    }

    FILE* fp = fopen(tmp_path, "wb");
    if (!fp) { free(tmp_path); return EDB_ERR_IO; }

    /* 预留 header */
    EdbHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    memcpy(hdr.magic, EDB_MAGIC, 8);
    hdr.version = EDB_VERSION;
    hdr.header_size = EDB_HEADER_SIZE;

    if (fseek(fp, EDB_HEADER_SIZE, SEEK_SET) != 0) { fclose(fp); free(tmp_path); return EDB_ERR_IO; }

    EdbSectionDesc secs[EDB_SEC_COUNT];
    memset(secs, 0, sizeof(secs));
    for (int i = 0; i < EDB_SEC_COUNT; i++) secs[i].section_id = (uint32_t)(i + 1);

    /* === 1. 字符串去重 + 写 ARCHIVE_RECORDS === */
    EdbStringDedup dedup;
    uint32_t dedup_buckets = archive_count * 4 + 64;
    uint32_t dedup_cap = archive_count * 128 + 4096;
    rc = edb_string_dedup_init(&dedup, dedup_buckets, dedup_cap);
    if (rc != EDB_OK) { fclose(fp); free(tmp_path); return rc; }

    secs[0].offset = (uint64_t)ftell(fp);
    secs[0].raw_size = (uint64_t)archive_count * sizeof(EdbDiskArchive);

    for (uint32_t i = 0; i < archive_count; i++) {
        uint32_t name_len = (uint32_t)strlen(archives[i].file_path);
        uint32_t name_off = 0;
        rc = edb_string_dedup_intern(&dedup, archives[i].file_path, name_len, &name_off);
        if (rc != EDB_OK) goto fail;
        rc = edb_write_disk_archive(fp, &archives[i], name_off, name_len);
        if (rc != EDB_OK) goto fail;
    }
    secs[0].encoded_size = (uint64_t)ftell(fp) - secs[0].offset;

    /* === 2. 写 ARCHIVE_STRING_POOL === */
    secs[1].offset = (uint64_t)ftell(fp);
    secs[1].raw_size = dedup.pool_size;
    if (fwrite(dedup.pool, 1, dedup.pool_size, fp) != dedup.pool_size) { rc = EDB_ERR_IO; goto fail; }
    secs[1].encoded_size = dedup.pool_size;
    edb_string_dedup_free(&dedup);

    /* === 3. 流式写 entry sections === */
    EdbPagedWriter detail_w, path_w, raw_w;
    rc = edb_paged_writer_init(&detail_w, fp, EDB_DETAIL_ENTRIES_PER_PAGE * (uint32_t)sizeof(EdbDiskEntryDetail));
    if (rc != EDB_OK) goto fail;
    rc = edb_paged_writer_init(&path_w, fp, EDB_PATH_PAGE_SIZE);
    if (rc != EDB_OK) { edb_paged_writer_free(&detail_w); goto fail; }
    rc = edb_paged_writer_init(&raw_w, fp, EDB_RAW_PAGE_SIZE);
    if (rc != EDB_OK) { edb_paged_writer_free(&detail_w); edb_paged_writer_free(&path_w); goto fail; }

    /* 记录每个 entry 的 core 和位置信息 */
    EdbDiskEntryCore* cores = (EdbDiskEntryCore*)malloc((entry_count + 1) * sizeof(EdbDiskEntryCore));
    if (!cores) {
        edb_paged_writer_free(&detail_w);
        edb_paged_writer_free(&path_w);
        rc = EDB_ERR_MEMORY; goto fail;
    }

    uint32_t path_offset = 0;
    uint32_t raw_offset = 0;

    /* ENTRY_CORE 起始位置 */
    uint64_t core_start = (uint64_t)ftell(fp);

    /* 先预留 core 数组空间 */
    if (fseek(fp, (long)(core_start + (uint64_t)entry_count * sizeof(EdbDiskEntryCore)), SEEK_SET) != 0) {
        free(cores); edb_paged_writer_free(&detail_w); edb_paged_writer_free(&path_w);
        rc = EDB_ERR_IO; goto fail;
    }

    /* 遍历 entries，写 detail/path/raw */
    uint32_t entry_idx = 0;
    if (entry_stream && entry_count > 0) {
        if (entry_stream->reset) entry_stream->reset(entry_stream->user_data);
        EdbEntryRecord rec;
        while (entry_stream->next(entry_stream->user_data, &rec) == 0 && entry_idx < entry_count) {
            cores[entry_idx].archive_id = rec.archive_id;
            cores[entry_idx].path_offset = path_offset;
            uint32_t epath_len = (uint32_t)strlen(rec.entry_path);
            cores[entry_idx].path_len = epath_len;

            /* 写入 path page */
            edb_paged_writer_append(&path_w, rec.entry_path, epath_len);
            path_offset += epath_len;

            /* 写入 detail */
            EdbDiskEntryDetail det;
            memset(&det, 0, sizeof(det));
            det.compressed_size = rec.compressed_size;
            det.original_size = rec.original_size;
            det.modified_time = rec.modified_time;

            if (rec.entry_raw_path && rec.entry_raw_path_len > 0) {
                det.raw_offset = raw_offset;
                det.raw_len = rec.entry_raw_path_len;
                edb_paged_writer_append(&raw_w, rec.entry_raw_path, rec.entry_raw_path_len);
                raw_offset += rec.entry_raw_path_len;
            } else {
                det.raw_offset = UINT32_MAX;
                det.raw_len = 0;
            }

            edb_paged_writer_append(&detail_w, &det, sizeof(det));
            entry_idx++;
        }
    }

    /* flush 所有分页写入器 */
    edb_paged_writer_flush(&detail_w);
    edb_paged_writer_flush(&path_w);
    edb_paged_writer_flush(&raw_w);

    /* 回写 entry cores */
    uint64_t detail_start = (uint64_t)ftell(fp);

    /* 写 detail pages */
    secs[3].offset = detail_start;
    secs[3].raw_size = edb_paged_writer_total_raw(&detail_w);
    secs[3].encoded_size = 0;
    secs[3].page_size = EDB_DETAIL_ENTRIES_PER_PAGE * (uint32_t)sizeof(EdbDiskEntryDetail);
    secs[3].aux_count = detail_w.page_count;
    for (uint32_t i = 0; i < detail_w.page_count; i++) {
        if (fwrite(&detail_w.pages[i], sizeof(EdbDiskPage), 1, fp) != 1) {
            free(cores); edb_paged_writer_free(&detail_w); edb_paged_writer_free(&path_w);
            rc = EDB_ERR_IO; goto fail;
        }
        secs[3].encoded_size += detail_w.pages[i].encoded_size;
    }
    secs[3].aux_offset = detail_start + secs[3].encoded_size;

    /* 写 path pages */
    uint64_t path_start = (uint64_t)ftell(fp);
    secs[4].offset = path_start;
    secs[4].raw_size = edb_paged_writer_total_raw(&path_w);
    secs[4].encoded_size = 0;
    secs[4].page_size = EDB_PATH_PAGE_SIZE;
    secs[4].aux_count = path_w.page_count;
    for (uint32_t i = 0; i < path_w.page_count; i++) {
        if (fwrite(&path_w.pages[i], sizeof(EdbDiskPage), 1, fp) != 1) {
            free(cores); edb_paged_writer_free(&detail_w); edb_paged_writer_free(&path_w);
            rc = EDB_ERR_IO; goto fail;
        }
        secs[4].encoded_size += path_w.pages[i].encoded_size;
    }
    secs[4].aux_offset = path_start + secs[4].encoded_size;

    /* 回写 core 数组 */
    {
        uint64_t cur = (uint64_t)ftell(fp);
        /* 用前面的 reserved 空间 */
        secs[2].offset = core_start;
        secs[2].raw_size = (uint64_t)entry_idx * sizeof(EdbDiskEntryCore);
        secs[2].encoded_size = secs[2].raw_size;
        fseek(fp, (long)core_start, SEEK_SET);
        fwrite(cores, sizeof(EdbDiskEntryCore), entry_idx, fp);
        fseek(fp, (long)cur, SEEK_SET);
    }
    free(cores);
    edb_paged_writer_free(&detail_w);
    edb_paged_writer_free(&path_w);
    edb_paged_writer_free(&raw_w);

    /* RAW_BLOBS section (暂时为空) */
    secs[5].offset = (uint64_t)ftell(fp);

    /* === 4. 构建倒排索引 === */
    uint32_t do_entry_index = (options && (options->flags & EDB_BUILD_ENTRY_INDEX)) ? 1 : 0;

    /* Archive postings */
    {
        EdbPostingBuilder builder;
        uint32_t bkts = archive_count * 4 + 256;
        rc = edb_postings_builder_init(&builder, bkts);
        if (rc != EDB_OK) goto fail;

        for (uint32_t i = 0; i < archive_count; i++) {
            edb_postings_count_text_grams(&builder, archives[i].file_path, i + 1);
        }
        rc = edb_postings_builder_prepare(&builder, archive_count);
        if (rc != EDB_OK) { edb_postings_builder_free(&builder); goto fail; }

        for (uint32_t i = 0; i < archive_count; i++) {
            edb_postings_fill_text_grams(&builder, archives[i].file_path, i + 1);
        }

        secs[6].offset = (uint64_t)ftell(fp);
        EdbDiskIndex* aindex = NULL;
        uint32_t aindex_count = 0;
        uint64_t awritten = 0;
        rc = edb_postings_write(fp, &builder, archive_count, &aindex, &aindex_count, &awritten);
        edb_postings_builder_free(&builder);
        if (rc != EDB_OK) { free(aindex); goto fail; }

        secs[6].encoded_size = awritten;
        secs[6].raw_size = awritten;

        /* Archive posting index */
        secs[7].offset = (uint64_t)ftell(fp);
        secs[7].raw_size = (uint64_t)aindex_count * sizeof(EdbDiskIndex);
        secs[7].encoded_size = secs[7].raw_size;
        if (aindex_count > 0) {
            fwrite(aindex, sizeof(EdbDiskIndex), aindex_count, fp);
        }
        free(aindex);
    }

    /* Entry postings */
    if (do_entry_index && entry_count > 0) {
        EdbPostingBuilder builder;
        uint32_t bkts = entry_count * 4 + 256;
        rc = edb_postings_builder_init(&builder, bkts);
        if (rc != EDB_OK) goto fail;

        /* 重新遍历 entries 收集路径 */
        if (entry_stream && entry_stream->reset) entry_stream->reset(entry_stream->user_data);
        EdbEntryRecord rec;
        uint32_t eid = 0;
        while (entry_stream->next(entry_stream->user_data, &rec) == 0 && eid < entry_count) {
            if (rec.entry_path)
                edb_postings_count_text_grams(&builder, rec.entry_path, eid + 1);
            eid++;
            if (eid % 500000 == 0) {
            }
        }

        rc = edb_postings_builder_prepare(&builder, entry_count);
        if (rc != EDB_OK) { edb_postings_builder_free(&builder); goto fail; }

        /* fill */
        if (entry_stream->reset) entry_stream->reset(entry_stream->user_data);
        eid = 0;
        while (entry_stream->next(entry_stream->user_data, &rec) == 0 && eid < entry_count) {
            if (rec.entry_path)
                edb_postings_fill_text_grams(&builder, rec.entry_path, eid + 1);
            eid++;
            if (eid % 500000 == 0) {
            }
        }

        secs[8].offset = (uint64_t)ftell(fp);
        EdbDiskIndex* eindex = NULL;
        uint32_t eindex_count = 0;
        uint64_t ewritten = 0;
        rc = edb_postings_write(fp, &builder, entry_count, &eindex, &eindex_count, &ewritten);
        edb_postings_builder_free(&builder);
        if (rc != EDB_OK) { free(eindex); goto fail; }

        secs[8].encoded_size = ewritten;
        secs[8].raw_size = ewritten;

        secs[9].offset = (uint64_t)ftell(fp);
        secs[9].raw_size = (uint64_t)eindex_count * sizeof(EdbDiskIndex);
        secs[9].encoded_size = secs[9].raw_size;
        if (eindex_count > 0) {
            fwrite(eindex, sizeof(EdbDiskIndex), eindex_count, fp);
        }
        free(eindex);
    }

    /* === 5. 写 section table + header === */
    uint64_t table_offset = 0;
    rc = edb_format_write_section_table(fp, secs, EDB_SEC_COUNT, &table_offset);
    if (rc != EDB_OK) goto fail;

    hdr.section_count = EDB_SEC_COUNT;
    hdr.archive_count = archive_count;
    hdr.active_archive_count = archive_count;
    hdr.entry_count = entry_count;
    hdr.active_entry_count = entry_count;
    hdr.section_table_offset = table_offset;

    rc = edb_format_write_header(fp, &hdr);
    if (rc != EDB_OK) goto fail;

    edb_file_sync(fp);
    fclose(fp);

    /* 原子重命名 */
    rc = edb_rename_atomic(tmp_path, output_path);
    free(tmp_path);
    return rc == 0 ? EDB_OK : EDB_ERR_IO;

fail:
    edb_string_dedup_free(&dedup);
    fclose(fp);
    remove(tmp_path);
    free(tmp_path);
    return rc;
}
