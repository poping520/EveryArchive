#pragma once

#include "ezdb_internal.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define EZDB_GRAM1 1u
#define EZDB_GRAM2 2u
#define EZDB_GRAM3 3u
#define EZDB_TOKEN_INLINE 0u
#define EZDB_TOKEN_HASHED 0x80000000u
#define EZDB_MAX_GRAM_TOKENS 3u
#define EZDB_STACK_TOKENS 512u
#define EZDB_STACK_KEYS (EZDB_STACK_TOKENS * EZDB_MAX_GRAM_TOKENS)

#define EZDB_POSTING_ARRAY 1u
#define EZDB_POSTING_RANGE 2u
#define EZDB_POSTING_BITSET 3u
#define EZDB_POSTING_TYPE_MASK 0x7fffffffu
#define EZDB_POSTING_COMPRESSED 0x80000000u
#define EZDB_BITSET_DENSITY_DIVISOR 16u

typedef struct PostingBuildEntry {
    uint32_t key;
    uint32_t* ids;
    uint32_t count;
    uint32_t cap;
    uint32_t next;
    uint32_t fill_mode;
    uint32_t fill_bytes;
} PostingBuildEntry;

typedef struct PostingBuilder {
    PostingBuildEntry* entries;
    uint32_t entry_count;
    uint32_t entry_cap;
    uint32_t* buckets;
    uint32_t bucket_count;
    uint32_t* id_block;
} PostingBuilder;

typedef struct PostingWriteStats {
    double sort_ms;
    double choose_ms;
    double encode_ms;
    double compress_ms;
    double fwrite_ms;
    double index_meta_ms;
    uint64_t raw_bytes;
    uint64_t encoded_bytes;
    uint32_t array_count;
    uint32_t range_count;
    uint32_t bitset_count;
    uint32_t compressed_count;
} PostingWriteStats;

typedef struct GramKeyCallback {
    int (*emit)(uint32_t key, void* user_data);
    void* user_data;
} GramKeyCallback;

int ezdb_postings_utf8_token_len(const unsigned char* s, size_t remain);
uint32_t ezdb_postings_make_gram_key_from_span(const char* text, uint32_t offset, uint32_t len, uint32_t token_count);
int ezdb_postings_enumerate_text_gram_keys(const char* text, const GramKeyCallback* callback);
int ezdb_postings_build_query_keys(const char* keyword, uint32_t** out_keys, uint32_t* out_count);
int ezdb_postings_builder_init(PostingBuilder* builder, uint32_t bucket_count);
void ezdb_postings_builder_free(PostingBuilder* builder);
int ezdb_postings_builder_add(PostingBuilder* builder, uint32_t key, uint32_t id);
int ezdb_postings_builder_add_count(PostingBuilder* builder, uint32_t key, uint32_t count);
PostingBuildEntry* ezdb_postings_builder_find(PostingBuilder* builder, uint32_t key);
int ezdb_postings_builder_prepare_fill(PostingBuilder* builder);
int ezdb_postings_builder_prepare_fill_adaptive(PostingBuilder* builder, uint32_t universe_count);
int ezdb_postings_builder_remove_id(PostingBuilder* builder, uint32_t key, uint32_t id);
int ezdb_postings_add_text_grams(PostingBuilder* builder, const char* text, uint32_t id, int mode);
int ezdb_postings_count_text_grams(PostingBuilder* builder, const char* text, uint32_t id);
int ezdb_postings_fill_text_grams(PostingBuilder* builder, const char* text, uint32_t id);
EzdbDiskIndex* ezdb_postings_find_index(EzdbDiskIndex* index, uint64_t count, uint32_t key);
int ezdb_postings_load(FILE* fp, uint64_t postings_offset, const EzdbDiskIndex* idx, uint32_t** out_ids);
int ezdb_postings_load_intersected(FILE* fp,
                                   uint64_t postings_offset,
                                   EzdbDiskIndex* index,
                                   uint64_t index_count,
                                   const uint32_t* keys,
                                   uint32_t key_count,
                                   uint32_t** out_ids,
                                   uint32_t* out_count);
int ezdb_postings_load_intersected_memory(PostingBuilder* builder,
                                          const uint32_t* keys,
                                          uint32_t key_count,
                                          uint32_t** out_ids,
                                          uint32_t* out_count);
int ezdb_postings_write(FILE* out,
                   PostingBuilder* builder,
                   uint32_t universe_count,
                   EzdbDiskIndex** out_index,
                   uint32_t* out_index_count,
                   uint64_t* out_written,
                   PostingWriteStats* stats);
