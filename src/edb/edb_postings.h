#pragma once

#include "edb_format.h"
#include <stdint.h>
#include <stdio.h>

/* ===== N-gram 常量 ===== */

#define EDB_GRAM1 1u
#define EDB_GRAM2 2u
#define EDB_GRAM3 3u
#define EDB_TOKEN_INLINE 0u
#define EDB_TOKEN_HASHED  0x80000000u
#define EDB_MAX_GRAM_TOKENS 3u

/* ===== Posting 容器类型 ===== */

#define EDB_POSTING_ARRAY   1u
#define EDB_POSTING_RANGE   2u
#define EDB_POSTING_BITSET  3u
#define EDB_POSTING_TYPE_MASK    0x7FFFFFFFu
#define EDB_POSTING_COMPRESSED   0x80000000u
#define EDB_BITSET_DENSITY_DIVISOR 16u

/* ===== Posting Builder ===== */

typedef struct {
    uint32_t key;
    uint32_t* ids;
    uint32_t count;
    uint32_t cap;
    uint32_t next;         /* 哈希链 */
    uint32_t fill_mode;    /* 0=数组, 1=bitset */
    uint32_t fill_bytes;   /* bitset 模式时的大小 */
    uint32_t last_id;      /* count 阶段去重用 */
} EdbPostingEntry;

typedef struct {
    EdbPostingEntry* entries;
    uint32_t entry_count;
    uint32_t entry_cap;
    uint32_t* buckets;
    uint32_t bucket_count;
    uint32_t* id_block;     /* prepare 阶段分配的连续 ID 存储 */
} EdbPostingBuilder;

/* ===== Gram Key 回调 ===== */

typedef struct {
    int (*emit)(uint32_t key, void* user_data);
    void* user_data;
} EdbGramKeyCallback;

/* ===== Query Key Collector ===== */

typedef struct {
    uint32_t* keys;
    uint32_t  count;
    uint32_t  cap;
} EdbQueryKeyCollector;

int edb_collect_query_key(uint32_t key, void* user_data);

uint32_t edb_intersect_sorted(const uint32_t* a, uint32_t a_count,
                               const uint32_t* b, uint32_t b_count,
                               uint32_t* out);

/* ===== N-gram 分词 ===== */

/* 获取 UTF-8 字符的字节长度 */
uint32_t edb_postings_utf8_char_len(const uint8_t* s, size_t remain);

/* 从文本的指定位置生成 gram key */
uint32_t edb_postings_make_gram_key(const char* text, uint32_t offset, uint32_t len,
                                     uint32_t token_count);

/* 枚举文本的所有 gram keys */
int edb_postings_enumerate_gram_keys(const char* text, const EdbGramKeyCallback* cb);

/* 为查询关键词生成 gram keys */
int edb_postings_build_query_keys(const char* keyword, uint32_t** out_keys,
                                   uint32_t* out_count);

/* ===== Posting Builder API ===== */

int  edb_postings_builder_init(EdbPostingBuilder* builder, uint32_t bucket_count);
void edb_postings_builder_free(EdbPostingBuilder* builder);

/* 添加 (key, id) 对（count 阶段） */
int edb_postings_builder_add(EdbPostingBuilder* builder, uint32_t key, uint32_t id);

/* 查找 entry */
EdbPostingEntry* edb_postings_builder_find(EdbPostingBuilder* builder, uint32_t key);

/* 准备 fill 阶段：分配 id_block */
int edb_postings_builder_prepare(EdbPostingBuilder* builder, uint32_t universe_count);

/* 为文本添加 gram（count 模式） */
int edb_postings_count_text_grams(EdbPostingBuilder* builder, const char* text, uint32_t id);

/* 为文本填充 gram（fill 模式） */
int edb_postings_fill_text_grams(EdbPostingBuilder* builder, const char* text, uint32_t id);

/* ===== Posting 写入 ===== */

int edb_postings_write(FILE* out, EdbPostingBuilder* builder, uint32_t universe_count,
                        EdbDiskIndex** out_index, uint32_t* out_index_count,
                        uint64_t* out_written);

/* ===== Posting 读取 ===== */

/* 加载单个 posting 列表 */
int edb_postings_load(const unsigned char* postings_base, uint64_t postings_size,
                       const EdbDiskIndex* idx, uint32_t** out_ids, uint32_t* out_count);

/* 加载多个 gram key 的 posting 并求交集 */
int edb_postings_load_intersected(const unsigned char* postings_base, uint64_t postings_size,
                                   EdbDiskIndex* index, uint32_t index_count,
                                   const uint32_t* keys, uint32_t key_count,
                                   uint32_t** out_ids, uint32_t* out_count);

/* 在 index 数组中查找 key */
EdbDiskIndex* edb_postings_find_index(EdbDiskIndex* index, uint32_t count, uint32_t key);

/* ===== 辅助：为文本做完整的 count+fill ===== */

int edb_postings_add_text_grams(EdbPostingBuilder* builder, const char* text, uint32_t id);
