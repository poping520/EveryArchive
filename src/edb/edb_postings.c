#include "edb_postings.h"
#include "edb_util.h"
#include <stdlib.h>
#include <string.h>

/* ===== N-gram 分词 ===== */

uint32_t edb_postings_utf8_char_len(const uint8_t* s, size_t remain) {
    return edb_utf8_char_len(s, remain);
}

static int edb_is_separator(uint32_t cp) {
    return cp == '/' || cp == '\\' || cp == '.';
}

/* 将 UTF-8 文本分词为 token 序列（跳过分隔符，ASCII 折叠） */
/* 返回 token 数量，tokens 写入 caller 提供的数组 */
/* 每个 token: (offset, byte_len) */
typedef struct { uint32_t offset; uint32_t len; } EdbTokenSpan;

static uint32_t edb_tokenize(const char* text, EdbTokenSpan* tokens, uint32_t max_tokens) {
    uint32_t count = 0;
    size_t slen = strlen(text);
    size_t i = 0;

    while (i < slen && count < max_tokens) {
        /* 跳过分隔符 */
        while (i < slen) {
            uint32_t clen = edb_utf8_char_len((const uint8_t*)text + i, slen - i);
            if (clen == 0) break;
            uint32_t cp = 0;
            const uint8_t* p = (const uint8_t*)text + i;
            if (clen == 1) cp = p[0];
            else if (clen == 2) cp = ((uint32_t)(p[0] & 0x1F) << 6) | (p[1] & 0x3F);
            else if (clen == 3) cp = ((uint32_t)(p[0] & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
            else if (clen == 4) cp = ((uint32_t)(p[0] & 0x07) << 18) | ((uint32_t)(p[1] & 0x3F) << 12) | ((uint32_t)(p[2] & 0x3F) << 6) | (p[3] & 0x3F);

            if (!edb_is_separator(cp)) break;
            i += clen;
        }
        if (i >= slen) break;

        /* 收集 token */
        uint32_t start = (uint32_t)i;
        while (i < slen) {
            uint32_t clen = edb_utf8_char_len((const uint8_t*)text + i, slen - i);
            if (clen == 0) break;
            const uint8_t* p = (const uint8_t*)text + i;
            uint32_t cp = 0;
            if (clen == 1) { cp = p[0]; }
            else if (clen == 2) cp = ((uint32_t)(p[0] & 0x1F) << 6) | (p[1] & 0x3F);
            else if (clen == 3) cp = ((uint32_t)(p[0] & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
            else if (clen == 4) cp = ((uint32_t)(p[0] & 0x07) << 18) | ((uint32_t)(p[1] & 0x3F) << 12) | ((uint32_t)(p[2] & 0x3F) << 6) | (p[3] & 0x3F);
            if (edb_is_separator(cp)) break;
            i += clen;
        }
        tokens[count].offset = start;
        tokens[count].len = (uint32_t)i - start;
        count++;
    }
    return count;
}

/* ASCII fold 文本到临时缓冲区 */
static void edb_fold_text(const char* text, uint32_t offset, uint32_t len, char* out) {
    for (uint32_t i = 0; i < len; i++) {
        out[i] = (char)edb_ascii_fold((uint8_t)text[offset + i]);
    }
}

uint32_t edb_postings_make_gram_key(const char* text, uint32_t offset, uint32_t len,
                                     uint32_t token_count) {
    if (len <= 3) {
        /* inline key */
        uint32_t key = 0;
        for (uint32_t i = 0; i < len; i++)
            key |= (uint32_t)edb_ascii_fold((uint8_t)text[offset + i]) << (i * 8);
        return key | (token_count << 24);
    }
    /* hashed key */
    char folded[256];
    if (len > sizeof(folded)) len = (uint32_t)sizeof(folded);
    edb_fold_text(text, offset, len, folded);
    uint32_t h = edb_fnv1a(folded, len);
    return EDB_TOKEN_HASHED | (token_count << 24) | (h & 0x00FFFFFFu);
}

int edb_postings_enumerate_gram_keys(const char* text, const EdbGramKeyCallback* cb) {
    EdbTokenSpan tokens[256];
    uint32_t ntokens = edb_tokenize(text, tokens, 256);
    if (ntokens == 0) return 0;

    int total = 0;
    for (uint32_t t = 0; t < ntokens; t++) {
        /* 1-gram */
        uint32_t key = edb_postings_make_gram_key(text, tokens[t].offset, tokens[t].len, 1);
        int rc = cb->emit(key, cb->user_data);
        if (rc < 0) return rc;
        total++;
    }
    for (uint32_t t = 0; t + 1 < ntokens; t++) {
        /* 2-gram: 连接 token[t] 和 token[t+1] */
        uint32_t off = tokens[t].offset;
        uint32_t len = tokens[t].len + tokens[t + 1].len;
        uint32_t key = edb_postings_make_gram_key(text, off, len, 2);
        int rc = cb->emit(key, cb->user_data);
        if (rc < 0) return rc;
        total++;
    }
    for (uint32_t t = 0; t + 2 < ntokens; t++) {
        /* 3-gram */
        uint32_t off = tokens[t].offset;
        uint32_t len = tokens[t].len + tokens[t + 1].len + tokens[t + 2].len;
        uint32_t key = edb_postings_make_gram_key(text, off, len, 3);
        int rc = cb->emit(key, cb->user_data);
        if (rc < 0) return rc;
        total++;
    }
    return total;
}

/* ===== Query Keys ===== */

int edb_collect_query_key(uint32_t key, void* user_data) {
    EdbQueryKeyCollector* c = (EdbQueryKeyCollector*)user_data;
    if (edb_ensure_cap((void**)&c->keys, &c->cap, c->count + 1, sizeof(uint32_t)) != 0)
        return -1;
    c->keys[c->count++] = key;
    return 0;
}

int edb_postings_build_query_keys(const char* keyword, uint32_t** out_keys,
                                   uint32_t* out_count) {
    EdbQueryKeyCollector col = {NULL, 0, 0};
    EdbGramKeyCallback cb;
    cb.emit = edb_collect_query_key;
    cb.user_data = &col;

    int rc = edb_postings_enumerate_gram_keys(keyword, &cb);
    if (rc < 0) { free(col.keys); return rc; }

    *out_keys = col.keys;
    *out_count = col.count;
    return 0;
}

/* ===== Posting Builder ===== */

int edb_postings_builder_init(EdbPostingBuilder* builder, uint32_t bucket_count) {
    memset(builder, 0, sizeof(*builder));
    builder->buckets = (uint32_t*)calloc(bucket_count, sizeof(uint32_t));
    if (!builder->buckets) return EDB_ERR_MEMORY;
    builder->bucket_count = bucket_count;
    return EDB_OK;
}

void edb_postings_builder_free(EdbPostingBuilder* builder) {
    if (builder->entries) {
        /* 如果 id_block 存在，ids 指向 id_block 内部，不需要单独释放 */
        if (!builder->id_block) {
            for (uint32_t i = 0; i < builder->entry_count; i++) {
                if (builder->entries[i].ids) free(builder->entries[i].ids);
            }
        }
        free(builder->entries);
    }
    free(builder->buckets);
    free(builder->id_block);
    memset(builder, 0, sizeof(*builder));
}

EdbPostingEntry* edb_postings_builder_find(EdbPostingBuilder* builder, uint32_t key) {
    uint32_t idx = edb_murmur3_final(key) % builder->bucket_count;
    uint32_t i = builder->buckets[idx];
    while (i != 0) {
        if (builder->entries[i - 1].key == key) return &builder->entries[i - 1];
        i = builder->entries[i - 1].next;
    }
    return NULL;
}

int edb_postings_builder_add(EdbPostingBuilder* builder, uint32_t key, uint32_t id) {
    EdbPostingEntry* e = edb_postings_builder_find(builder, key);
    if (e) {
        if (e->count > 0 && e->last_id == id) return 0; /* 去重 */
        e->last_id = id;
        if (e->count >= e->cap) {
            uint32_t new_cap = e->cap ? e->cap * 2 : 16;
            uint32_t* p = (uint32_t*)realloc(e->ids, new_cap * sizeof(uint32_t));
            if (!p) return EDB_ERR_MEMORY;
            e->ids = p;
            e->cap = new_cap;
        }
        e->ids[e->count++] = id;
        return 0;
    }

    /* 新 entry */
    if (edb_ensure_cap((void**)&builder->entries, &builder->entry_cap,
                        builder->entry_count + 1, sizeof(EdbPostingEntry)) != 0)
        return EDB_ERR_MEMORY;

    EdbPostingEntry* ne = &builder->entries[builder->entry_count];
    memset(ne, 0, sizeof(*ne));
    ne->key = key;
    ne->ids = (uint32_t*)malloc(16 * sizeof(uint32_t));
    if (!ne->ids) return EDB_ERR_MEMORY;
    ne->cap = 16;
    ne->ids[0] = id;
    ne->count = 1;
    ne->last_id = id;

    uint32_t idx = edb_murmur3_final(key) % builder->bucket_count;
    ne->next = builder->buckets[idx];
    builder->buckets[idx] = builder->entry_count + 1;

    builder->entry_count++;
    return 0;
}

int edb_postings_builder_prepare(EdbPostingBuilder* builder, uint32_t universe_count) {
    /* 计算总 ID 数量并分配连续存储 */
    uint32_t total_ids = 0;
    for (uint32_t i = 0; i < builder->entry_count; i++) {
        EdbPostingEntry* e = &builder->entries[i];
        if (e->count >= universe_count / EDB_BITSET_DENSITY_DIVISOR) {
            e->fill_mode = 1;
            e->fill_bytes = (universe_count + 7) / 8;
            total_ids += e->fill_bytes;
        } else {
            e->fill_mode = 0;
            total_ids += e->count * sizeof(uint32_t);
        }
    }

    if (total_ids > 0) {
        builder->id_block = (uint32_t*)calloc(total_ids, 1);
        if (!builder->id_block) return EDB_ERR_MEMORY;
    }

    /* 分配每个 entry 的 id_block 切片，释放旧的独立 ids */
    uint32_t* ptr = builder->id_block;
    for (uint32_t i = 0; i < builder->entry_count; i++) {
        EdbPostingEntry* e = &builder->entries[i];
        free(e->ids); /* 释放 count 阶段分配的独立数组 */
        if (e->fill_mode == 1) {
            e->ids = ptr;
            ptr = (uint32_t*)((uint8_t*)ptr + e->fill_bytes);
        } else {
            e->ids = ptr;
            ptr += e->count;
        }
        e->count = 0; /* reset count for fill phase */
        e->last_id = 0;
    }
    return EDB_OK;
}

int edb_postings_count_text_grams(EdbPostingBuilder* builder, const char* text, uint32_t id) {
    EdbGramKeyCallback cb;
    cb.emit = NULL;
    cb.user_data = NULL;

    EdbTokenSpan tokens[256];
    uint32_t ntokens = edb_tokenize(text, tokens, 256);

    for (uint32_t t = 0; t < ntokens; t++) {
        uint32_t key = edb_postings_make_gram_key(text, tokens[t].offset, tokens[t].len, 1);
        int rc = edb_postings_builder_add(builder, key, id);
        if (rc != 0) return rc;
    }
    for (uint32_t t = 0; t + 1 < ntokens; t++) {
        uint32_t off = tokens[t].offset;
        uint32_t len = tokens[t].len + tokens[t + 1].len;
        uint32_t key = edb_postings_make_gram_key(text, off, len, 2);
        int rc = edb_postings_builder_add(builder, key, id);
        if (rc != 0) return rc;
    }
    for (uint32_t t = 0; t + 2 < ntokens; t++) {
        uint32_t off = tokens[t].offset;
        uint32_t len = tokens[t].len + tokens[t + 1].len + tokens[t + 2].len;
        uint32_t key = edb_postings_make_gram_key(text, off, len, 3);
        int rc = edb_postings_builder_add(builder, key, id);
        if (rc != 0) return rc;
    }
    return 0;
}

int edb_postings_fill_text_grams(EdbPostingBuilder* builder, const char* text, uint32_t id) {
    EdbTokenSpan tokens[256];
    uint32_t ntokens = edb_tokenize(text, tokens, 256);

    for (uint32_t t = 0; t < ntokens; t++) {
        uint32_t key = edb_postings_make_gram_key(text, tokens[t].offset, tokens[t].len, 1);
        EdbPostingEntry* e = edb_postings_builder_find(builder, key);
        if (e) {
            if (e->fill_mode == 1) {
                edb_bit_set((unsigned char*)e->ids, id);
            } else {
                e->ids[e->count++] = id;
            }
        }
    }
    for (uint32_t t = 0; t + 1 < ntokens; t++) {
        uint32_t off = tokens[t].offset;
        uint32_t len = tokens[t].len + tokens[t + 1].len;
        uint32_t key = edb_postings_make_gram_key(text, off, len, 2);
        EdbPostingEntry* e = edb_postings_builder_find(builder, key);
        if (e) {
            if (e->fill_mode == 1) {
                edb_bit_set((unsigned char*)e->ids, id);
            } else {
                e->ids[e->count++] = id;
            }
        }
    }
    for (uint32_t t = 0; t + 2 < ntokens; t++) {
        uint32_t off = tokens[t].offset;
        uint32_t len = tokens[t].len + tokens[t + 1].len + tokens[t + 2].len;
        uint32_t key = edb_postings_make_gram_key(text, off, len, 3);
        EdbPostingEntry* e = edb_postings_builder_find(builder, key);
        if (e) {
            if (e->fill_mode == 1) {
                edb_bit_set((unsigned char*)e->ids, id);
            } else {
                e->ids[e->count++] = id;
            }
        }
    }
    return 0;
}

int edb_postings_add_text_grams(EdbPostingBuilder* builder, const char* text, uint32_t id) {
    return edb_postings_count_text_grams(builder, text, id);
}

/* ===== 自适应编码 ===== */

static int cmp_uint32(const void* a, const void* b) {
    uint32_t va = *(const uint32_t*)a, vb = *(const uint32_t*)b;
    return (va > vb) - (va < vb);
}

/* delta varint 编码 */
static uint32_t edb_encode_array(const uint32_t* ids, uint32_t count, uint8_t* buf) {
    uint32_t pos = 0;
    uint32_t prev = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t delta = ids[i] - prev;
        prev = ids[i];
        pos += (uint32_t)edb_write_varuint(buf + pos, delta);
    }
    return pos;
}

/* range 编码：(start, count) 对 */
static uint32_t edb_encode_range(const uint32_t* ids, uint32_t count, uint8_t* buf) {
    uint32_t pos = 0;
    uint32_t i = 0;
    while (i < count) {
        uint32_t start = ids[i];
        uint32_t run = 1;
        while (i + run < count && ids[i + run] == ids[i + run - 1] + 1) run++;
        /* 写入 (start, run) */
        memcpy(buf + pos, &start, 4); pos += 4;
        memcpy(buf + pos, &run, 4); pos += 4;
        i += run;
    }
    return pos;
}

/* ===== Posting 写入 ===== */

int edb_postings_write(FILE* out, EdbPostingBuilder* builder, uint32_t universe_count,
                        EdbDiskIndex** out_index, uint32_t* out_index_count,
                        uint64_t* out_written) {
    uint32_t n = builder->entry_count;
    if (n == 0) {
        *out_index = NULL;
        *out_index_count = 0;
        *out_written = 0;
        return EDB_OK;
    }

    /* 排序 entries by key */
    qsort(builder->entries, n, sizeof(EdbPostingEntry),
          (int (*)(const void*, const void*))cmp_uint32);

    EdbDiskIndex* index = (EdbDiskIndex*)calloc(n, sizeof(EdbDiskIndex));
    if (!index) return EDB_ERR_MEMORY;

    size_t buf_cap = 256 * 1024;
    uint8_t* enc_buf = (uint8_t*)malloc(buf_cap);
    uint8_t* range_buf = (uint8_t*)malloc(buf_cap);
    if (!enc_buf || !range_buf) {
        free(enc_buf); free(range_buf); free(index);
        return EDB_ERR_MEMORY;
    }

    uint64_t postings_start = (uint64_t)ftell(out);
    uint64_t offset = 0;

    for (uint32_t i = 0; i < n; i++) {
        EdbPostingEntry* e = &builder->entries[i];
        uint32_t count = e->fill_mode == 1 ? universe_count : e->count;

        /* 排序 IDs（非 bitset 模式） */
        if (e->fill_mode == 0 && e->count > 1) {
            qsort(e->ids, e->count, sizeof(uint32_t), cmp_uint32);
            count = e->count;
        }

        /* 选择编码 */
        uint32_t enc_size = 0;
        uint32_t container_type = EDB_POSTING_ARRAY;
        uint32_t raw_size = 0;
        uint8_t* final_buf = NULL;
        int compressed = 0;

        /* 尝试 array 编码 */
        if (e->fill_mode == 1) {
            /* bitset: 直接使用 id_block 中的 bitset 数据 */
            container_type = EDB_POSTING_BITSET;
            enc_size = e->fill_bytes;
            final_buf = (uint8_t*)e->ids;
            raw_size = enc_size;
        } else {
            /* 确保缓冲区足够大：array 最多 count*5 字节，range 最多 count*8 字节 */
            size_t needed = (size_t)count * 8 + 64;
            if (needed > buf_cap) {
                uint8_t* tmp = (uint8_t*)realloc(enc_buf, needed);
                if (!tmp) { free(index); free(enc_buf); free(range_buf); return EDB_ERR_MEMORY; }
                enc_buf = tmp;
                tmp = (uint8_t*)realloc(range_buf, needed);
                if (!tmp) { free(index); free(enc_buf); free(range_buf); return EDB_ERR_MEMORY; }
                range_buf = tmp;
                buf_cap = needed;
            }
            enc_size = edb_encode_array(e->ids, count, enc_buf);
            container_type = EDB_POSTING_ARRAY;

            /* 尝试 range 编码 */
            uint32_t range_size = edb_encode_range(e->ids, count, range_buf);
            if (range_size < enc_size && range_size <= count * 4u / 2u) {
                memcpy(enc_buf, range_buf, range_size);
                enc_size = range_size;
                container_type = EDB_POSTING_RANGE;
            }
            final_buf = enc_buf;
            raw_size = enc_size;
        }

        /* 尝试 zlib 压缩 */
        if (raw_size >= EDB_COMPRESS_MIN_SIZE) {
            uint8_t* comp_buf = NULL;
            uint32_t comp_size = 0;
            int rc = edb_format_compress(final_buf, raw_size, &comp_buf, &comp_size);
            if (rc == 0 && comp_buf) {
                final_buf = comp_buf;
                enc_size = comp_size;
                compressed = 1;
            }
        }

        /* 写入 */
        if (fwrite(final_buf, 1, enc_size, out) != enc_size) {
            if (compressed) free(final_buf);
            free(index); free(enc_buf); free(range_buf);
            return EDB_ERR_IO;
        }

        index[i].key = e->key;
        index[i].count = e->fill_mode == 1 ? (uint32_t)universe_count : count;
        index[i].container_type = container_type | (compressed ? EDB_POSTING_COMPRESSED : 0);
        index[i].encoded_size = enc_size;
        index[i].raw_size = raw_size;
        index[i].offset = offset;

        offset += enc_size;
        if (compressed) free(final_buf);
    }

    free(enc_buf);
    free(range_buf);
    *out_index = index;
    *out_index_count = n;
    *out_written = offset;
    return EDB_OK;
}

/* ===== Posting 读取 ===== */

EdbDiskIndex* edb_postings_find_index(EdbDiskIndex* index, uint32_t count, uint32_t key) {
    /* 线性查找，小规模足够 */
    for (uint32_t i = 0; i < count; i++) {
        if (index[i].key == key) return &index[i];
    }
    return NULL;
}

static int edb_decode_posting(const unsigned char* base, uint64_t postings_size,
                               const EdbDiskIndex* idx,
                               uint32_t** out_ids, uint32_t* out_count) {
    if (idx->offset + idx->encoded_size > postings_size) return EDB_ERR_FORMAT;

    const uint8_t* payload = base + idx->offset;
    uint32_t container = idx->container_type & EDB_POSTING_TYPE_MASK;
    int compressed = (idx->container_type & EDB_POSTING_COMPRESSED) != 0;

    uint8_t* data = NULL;
    uint32_t data_len = idx->raw_size;

    if (compressed) {
        data = (uint8_t*)malloc(idx->raw_size);
        if (!data) return EDB_ERR_MEMORY;
        uint32_t dec_len = 0;
        int rc = edb_format_decompress(payload, idx->encoded_size, data, idx->raw_size, &dec_len);
        if (rc != EDB_OK) { free(data); return rc; }
    } else {
        data = (uint8_t*)payload;
    }

    uint32_t* ids = NULL;
    uint32_t count = 0;

    if (container == EDB_POSTING_ARRAY) {
        /* delta varint 解码 */
        uint32_t cap = idx->count;
        ids = (uint32_t*)malloc(cap * sizeof(uint32_t));
        if (!ids) { if (compressed) free(data); return EDB_ERR_MEMORY; }

        const uint8_t* p = data;
        uint32_t remain = data_len;
        uint32_t prev = 0;
        while (count < cap && remain > 0) {
            uint32_t delta;
            int n = edb_read_varuint(p, remain, &delta);
            if (n < 0) break;
            prev += delta;
            ids[count++] = prev;
            p += n;
            remain -= (uint32_t)n;
        }
    } else if (container == EDB_POSTING_RANGE) {
        /* (start, run) pairs */
        uint32_t cap = idx->count;
        ids = (uint32_t*)malloc(cap * sizeof(uint32_t));
        if (!ids) { if (compressed) free(data); return EDB_ERR_MEMORY; }

        const uint8_t* p = data;
        uint32_t remain = data_len;
        while (remain >= 8 && count < cap) {
            uint32_t start, run;
            memcpy(&start, p, 4); p += 4;
            memcpy(&run, p, 4); p += 4;
            remain -= 8;
            for (uint32_t j = 0; j < run && count < cap; j++)
                ids[count++] = start + j;
        }
    } else if (container == EDB_POSTING_BITSET) {
        /* bitset -> ID list */
        uint32_t universe = idx->count;
        ids = (uint32_t*)malloc(universe * sizeof(uint32_t));
        if (!ids) { if (compressed) free(data); return EDB_ERR_MEMORY; }

        for (uint32_t i = 0; i < universe; i++) {
            if (edb_bit_get((const unsigned char*)data, i))
                ids[count++] = i;
        }
    }

    if (compressed) free(data);

    *out_ids = ids;
    *out_count = count;
    return EDB_OK;
}

int edb_postings_load(const unsigned char* postings_base, uint64_t postings_size,
                       const EdbDiskIndex* idx, uint32_t** out_ids, uint32_t* out_count) {
    return edb_decode_posting(postings_base, postings_size, idx, out_ids, out_count);
}

/* 两个有序数组的交集 */
uint32_t edb_intersect_sorted(const uint32_t* a, uint32_t a_count,
                               const uint32_t* b, uint32_t b_count,
                               uint32_t* out) {
    uint32_t i = 0, j = 0, k = 0;
    while (i < a_count && j < b_count) {
        if (a[i] == b[j]) { out[k++] = a[i]; i++; j++; }
        else if (a[i] < b[j]) i++;
        else j++;
    }
    return k;
}

int edb_postings_load_intersected(const unsigned char* postings_base, uint64_t postings_size,
                                   EdbDiskIndex* index, uint32_t index_count,
                                   const uint32_t* keys, uint32_t key_count,
                                   uint32_t** out_ids, uint32_t* out_count) {
    if (key_count == 0) { *out_ids = NULL; *out_count = 0; return EDB_OK; }

    uint32_t* result = NULL;
    uint32_t result_count = 0;

    for (uint32_t k = 0; k < key_count; k++) {
        EdbDiskIndex* idx = edb_postings_find_index(index, index_count, keys[k]);
        if (!idx) {
            /* gram key 不存在，交集为空 */
            free(result);
            *out_ids = NULL;
            *out_count = 0;
            return EDB_OK;
        }

        uint32_t* ids = NULL;
        uint32_t count = 0;
        int rc = edb_decode_posting(postings_base, postings_size, idx, &ids, &count);
        if (rc != EDB_OK) { free(result); return rc; }

        if (k == 0) {
            result = ids;
            result_count = count;
        } else {
            uint32_t* new_result = (uint32_t*)malloc(
                (result_count < count ? result_count : count) * sizeof(uint32_t));
            if (!new_result) { free(result); free(ids); return EDB_ERR_MEMORY; }
            uint32_t new_count = edb_intersect_sorted(result, result_count, ids, count, new_result);
            free(result);
            free(ids);
            result = new_result;
            result_count = new_count;
        }

        if (result_count == 0) break;
    }

    *out_ids = result;
    *out_count = result_count;
    return EDB_OK;
}
