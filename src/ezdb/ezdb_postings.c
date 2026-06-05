#include "ezdb_postings.h"
#include "ezdb_io.h"

#include <zlib.h>

#include <stdlib.h>
#include <string.h>
#if defined(_MSC_VER)
#include <intrin.h>
#endif
#include <windows.h>

static unsigned char ezdb_postings_fold_ascii_byte(unsigned char ch)
{
    return (ch >= 'A' && ch <= 'Z') ? (unsigned char)(ch + ('a' - 'A')) : ch;
}

static uint32_t ezdb_postings_fnv1a_bytes(const char* text, size_t len)
{
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < len; ++i) {
        hash ^= (unsigned char)text[i];
        hash *= 16777619u;
    }
    return hash;
}

static int ezdb_postings_u32_compare(const void* a, const void* b)
{
    uint32_t av = *(const uint32_t*)a;
    uint32_t bv = *(const uint32_t*)b;
    return (av > bv) - (av < bv);
}

static int posting_entry_key_compare(const void* a, const void* b)
{
    const PostingBuildEntry* pa = *(const PostingBuildEntry* const*)a;
    const PostingBuildEntry* pb = *(const PostingBuildEntry* const*)b;
    if (pa->key == pb->key) return 0;
    return pa->key < pb->key ? -1 : 1;
}

typedef struct EzdbPostingQueryIndex {
    EzdbDiskIndex* idx;
} EzdbPostingQueryIndex;

static int ezdb_postings_index_compare(const void* a, const void* b)
{
    const EzdbDiskIndex* ia = (const EzdbDiskIndex*)a;
    const EzdbDiskIndex* ib = (const EzdbDiskIndex*)b;
    if (ia->key == ib->key) return 0;
    return ia->key < ib->key ? -1 : 1;
}

static int ezdb_postings_query_index_compare(const void* a, const void* b)
{
    const EzdbPostingQueryIndex* qa = (const EzdbPostingQueryIndex*)a;
    const EzdbPostingQueryIndex* qb = (const EzdbPostingQueryIndex*)b;
    if (qa->idx->count == qb->idx->count) return 0;
    return qa->idx->count < qb->idx->count ? -1 : 1;
}

static double ezdb_postings_now_ms(void)
{
    LARGE_INTEGER freq;
    LARGE_INTEGER counter;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart * 1000.0 / (double)freq.QuadPart;
}

static int ezdb_postings_ensure_capacity(void** data, size_t elem_size, uint32_t* capacity, uint32_t needed)
{
    if (*capacity >= needed) return EZDB_OK;
    uint32_t next = *capacity ? *capacity : 1024u;
    while (next < needed) {
        if (next > UINT32_MAX / 2u) return EZDB_ERR_MEMORY;
        next *= 2u;
    }
    void* new_data = realloc(*data, elem_size * (size_t)next);
    if (!new_data) return EZDB_ERR_MEMORY;
    *data = new_data;
    *capacity = next;
    return EZDB_OK;
}

static int ezdb_postings_ensure_capacity_small(void** data, size_t elem_size, uint32_t* capacity, uint32_t needed)
{
    if (*capacity >= needed) return EZDB_OK;
    uint32_t next = *capacity ? *capacity : 4u;
    while (next < needed) {
        uint32_t grow = next < 1024u ? next : next / 2u;
        if (!grow) grow = 1u;
        if (next > UINT32_MAX - grow) return EZDB_ERR_MEMORY;
        next += grow;
    }
    void* new_data = realloc(*data, elem_size * (size_t)next);
    if (!new_data) return EZDB_ERR_MEMORY;
    *data = new_data;
    *capacity = next;
    return EZDB_OK;
}

static uint32_t posting_bucket_for(uint32_t key, uint32_t bucket_count)
{
    uint32_t x = key;
    x ^= x >> 16u;
    x *= 0x7feb352du;
    x ^= x >> 15u;
    x *= 0x846ca68bu;
    x ^= x >> 16u;
    return x & (bucket_count - 1u);
}

int ezdb_postings_utf8_token_len(const unsigned char* s, size_t remain)
{
    if (!remain) return 0;
    unsigned char ch = s[0];
    if (ch < 0x80u) return 1;
    if ((ch & 0xe0u) == 0xc0u && remain >= 2 && (s[1] & 0xc0u) == 0x80u) return 2;
    if ((ch & 0xf0u) == 0xe0u && remain >= 3 && (s[1] & 0xc0u) == 0x80u && (s[2] & 0xc0u) == 0x80u) return 3;
    if ((ch & 0xf8u) == 0xf0u && remain >= 4 && (s[1] & 0xc0u) == 0x80u && (s[2] & 0xc0u) == 0x80u && (s[3] & 0xc0u) == 0x80u) return 4;
    return 1;
}

static int ezdb_postings_split_tokens(const char* text, uint32_t len, uint32_t** out_offsets, uint32_t** out_lens, uint32_t* out_count)
{
    uint32_t cap = 0;
    uint32_t count = 0;
    uint32_t* offsets = NULL;
    uint32_t* lens = NULL;
    uint32_t i = 0;
    while (i < len) {
        int token_len = ezdb_postings_utf8_token_len((const unsigned char*)text + i, (size_t)(len - i));
        if (count + 1 > cap) {
            uint32_t next = cap ? cap * 2u : 32u;
            uint32_t* new_offsets;
            uint32_t* new_lens;
            while (next < count + 1) {
                if (next > UINT32_MAX / 2u) {
                    free(offsets);
                    free(lens);
                    return EZDB_ERR_MEMORY;
                }
                next *= 2u;
            }
            new_offsets = (uint32_t*)realloc(offsets, sizeof(uint32_t) * (size_t)next);
            if (!new_offsets) {
                free(offsets);
                free(lens);
                return EZDB_ERR_MEMORY;
            }
            offsets = new_offsets;
            new_lens = (uint32_t*)realloc(lens, sizeof(uint32_t) * (size_t)next);
            if (!new_lens) {
                free(offsets);
                free(lens);
                return EZDB_ERR_MEMORY;
            }
            lens = new_lens;
            cap = next;
        }
        offsets[count] = i;
        lens[count] = (uint32_t)token_len;
        ++count;
        i += (uint32_t)token_len;
    }
    *out_offsets = offsets;
    *out_lens = lens;
    *out_count = count;
    return EZDB_OK;
}

uint32_t ezdb_postings_make_gram_key_from_span(const char* text, uint32_t offset, uint32_t len, uint32_t token_count)
{
    const unsigned char* s = (const unsigned char*)text + offset;
    if (len <= 3u) {
        uint32_t value = 0;
        for (uint32_t i = 0; i < len; ++i) value = (value << 8) | (uint32_t)ezdb_postings_fold_ascii_byte(s[i]);
        return (EZDB_TOKEN_INLINE << 31) | (token_count << 24) | value;
    }
    uint32_t hash = ezdb_postings_fnv1a_bytes(text + offset, len) & 0x00ffffffu;
    return EZDB_TOKEN_HASHED | (token_count << 24) | hash;
}

int ezdb_postings_enumerate_text_gram_keys(const char* text, const GramKeyCallback* callback)
{
    if (!text || !callback || !callback->emit) return EZDB_ERR_ARG;
    uint32_t len = (uint32_t)strlen(text);
    if (!len) return EZDB_OK;
    uint32_t stack_offsets[EZDB_STACK_TOKENS];
    uint32_t stack_lens[EZDB_STACK_TOKENS];
    uint32_t stack_keys[EZDB_STACK_KEYS];
    uint32_t* offsets = stack_offsets;
    uint32_t* lens = stack_lens;
    uint32_t* keys = stack_keys;
    uint32_t token_cap = EZDB_STACK_TOKENS;
    uint32_t key_cap = EZDB_STACK_KEYS;
    uint32_t token_count = 0;
    uint32_t key_count = 0;
    uint32_t pos = 0;
    while (pos < len) {
        int token_len = ezdb_postings_utf8_token_len((const unsigned char*)text + pos, (size_t)(len - pos));
        if (token_count >= token_cap) {
            uint32_t next_cap = token_cap * 2u;
            uint32_t* new_offsets = (uint32_t*)malloc(sizeof(uint32_t) * (size_t)next_cap);
            uint32_t* new_lens = (uint32_t*)malloc(sizeof(uint32_t) * (size_t)next_cap);
            if (!new_offsets || !new_lens) {
                free(new_offsets);
                free(new_lens);
                if (offsets != stack_offsets) free(offsets);
                if (lens != stack_lens) free(lens);
                return EZDB_ERR_MEMORY;
            }
            memcpy(new_offsets, offsets, sizeof(uint32_t) * (size_t)token_count);
            memcpy(new_lens, lens, sizeof(uint32_t) * (size_t)token_count);
            if (offsets != stack_offsets) free(offsets);
            if (lens != stack_lens) free(lens);
            offsets = new_offsets;
            lens = new_lens;
            token_cap = next_cap;
        }
        offsets[token_count] = pos;
        lens[token_count] = (uint32_t)token_len;
        ++token_count;
        pos += (uint32_t)token_len;
    }
    if (token_count * EZDB_MAX_GRAM_TOKENS > key_cap) {
        key_cap = token_count * EZDB_MAX_GRAM_TOKENS;
        keys = (uint32_t*)malloc(sizeof(uint32_t) * (size_t)key_cap);
        if (!keys) {
            if (offsets != stack_offsets) free(offsets);
            if (lens != stack_lens) free(lens);
            return EZDB_ERR_MEMORY;
        }
    }
    for (uint32_t kind = EZDB_GRAM1; kind <= EZDB_GRAM3; ++kind) {
        if (token_count < kind) continue;
        for (uint32_t i = 0; i + kind <= token_count; ++i) {
            uint32_t byte_start = offsets[i];
            uint32_t byte_end = offsets[i + kind - 1u] + lens[i + kind - 1u];
            keys[key_count++] = ezdb_postings_make_gram_key_from_span(text, byte_start, byte_end - byte_start, kind);
        }
    }
    if (offsets != stack_offsets) free(offsets);
    if (lens != stack_lens) free(lens);
    uint32_t seen_stack[EZDB_STACK_KEYS];
    uint32_t* seen_keys = key_count <= EZDB_STACK_KEYS ? seen_stack : (uint32_t*)malloc(sizeof(uint32_t) * (size_t)key_count);
    if (!seen_keys) {
        if (keys != stack_keys) free(keys);
        return EZDB_ERR_MEMORY;
    }
    uint32_t seen_count = 0;
    for (uint32_t i = 0; i < key_count; ++i) {
        int duplicate = 0;
        for (uint32_t j = 0; j < seen_count; ++j) {
            if (seen_keys[j] == keys[i]) {
                duplicate = 1;
                break;
            }
        }
        if (duplicate) continue;
        seen_keys[seen_count++] = keys[i];
        int rc = callback->emit(keys[i], callback->user_data);
        if (rc != EZDB_OK) {
            if (seen_keys != seen_stack) free(seen_keys);
            if (keys != stack_keys) free(keys);
            return rc;
        }
    }
    if (seen_keys != seen_stack) free(seen_keys);
    if (keys != stack_keys) free(keys);
    return EZDB_OK;
}

int ezdb_postings_build_query_keys(const char* keyword, uint32_t** out_keys, uint32_t* out_count)
{
    uint32_t len = (uint32_t)strlen(keyword);
    if (!len) return EZDB_ERR_ARG;
    uint32_t* offsets = NULL;
    uint32_t* lens = NULL;
    uint32_t token_count = 0;
    int rc = ezdb_postings_split_tokens(keyword, len, &offsets, &lens, &token_count);
    if (rc != EZDB_OK) return rc;
    uint32_t kind = token_count >= EZDB_GRAM3 ? EZDB_GRAM3 : token_count;
    uint32_t count = token_count - kind + 1u;
    uint32_t* keys = (uint32_t*)malloc(sizeof(uint32_t) * count);
    if (!keys) {
        free(offsets);
        free(lens);
        return EZDB_ERR_MEMORY;
    }
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t byte_start = offsets[i];
        uint32_t byte_end = offsets[i + kind - 1u] + lens[i + kind - 1u];
        keys[i] = ezdb_postings_make_gram_key_from_span(keyword, byte_start, byte_end - byte_start, kind);
    }
    free(offsets);
    free(lens);
    qsort(keys, count, sizeof(uint32_t), ezdb_postings_u32_compare);
    uint32_t n = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (i && keys[i] == keys[i - 1]) continue;
        keys[n++] = keys[i];
    }
    *out_keys = keys;
    *out_count = n;
    return EZDB_OK;
}

int ezdb_postings_builder_init(PostingBuilder* builder, uint32_t bucket_count)
{
    if (!builder || !bucket_count) return EZDB_ERR_ARG;
    memset(builder, 0, sizeof(*builder));
    builder->bucket_count = bucket_count;
    builder->buckets = (uint32_t*)malloc(sizeof(uint32_t) * (size_t)bucket_count);
    if (!builder->buckets) return EZDB_ERR_MEMORY;
    for (uint32_t i = 0; i < bucket_count; ++i) builder->buckets[i] = UINT32_MAX;
    return EZDB_OK;
}

void ezdb_postings_builder_free(PostingBuilder* builder)
{
    if (!builder) return;
    if (builder->id_block) {
        free(builder->id_block);
    } else {
        for (uint32_t i = 0; i < builder->entry_count; ++i) free(builder->entries[i].ids);
    }
    free(builder->entries);
    free(builder->buckets);
    memset(builder, 0, sizeof(*builder));
}

int ezdb_postings_builder_add(PostingBuilder* builder, uint32_t key, uint32_t id)
{
    uint32_t bucket = posting_bucket_for(key, builder->bucket_count);
    for (uint32_t i = builder->buckets[bucket]; i != UINT32_MAX; i = builder->entries[i].next) {
        PostingBuildEntry* entry = &builder->entries[i];
        if (entry->key == key) {
            if (entry->count && entry->ids[entry->count - 1u] == id) return EZDB_OK;
            if (ezdb_postings_ensure_capacity_small((void**)&entry->ids, sizeof(uint32_t), &entry->cap, entry->count + 1u) != EZDB_OK) return EZDB_ERR_MEMORY;
            entry->ids[entry->count++] = id;
            return EZDB_OK;
        }
    }

    if (ezdb_postings_ensure_capacity((void**)&builder->entries, sizeof(PostingBuildEntry), &builder->entry_cap, builder->entry_count + 1u) != EZDB_OK) {
        return EZDB_ERR_MEMORY;
    }
    PostingBuildEntry* entry = &builder->entries[builder->entry_count];
    memset(entry, 0, sizeof(*entry));
    entry->key = key;
    entry->next = builder->buckets[bucket];
    builder->buckets[bucket] = builder->entry_count;
    builder->entry_count += 1u;
    if (ezdb_postings_ensure_capacity_small((void**)&entry->ids, sizeof(uint32_t), &entry->cap, 1u) != EZDB_OK) return EZDB_ERR_MEMORY;
    entry->ids[entry->count++] = id;
    return EZDB_OK;
}

PostingBuildEntry* ezdb_postings_builder_find(PostingBuilder* builder, uint32_t key)
{
    if (!builder || !builder->buckets || !builder->bucket_count) return NULL;
    uint32_t bucket = posting_bucket_for(key, builder->bucket_count);
    for (uint32_t i = builder->buckets[bucket]; i != UINT32_MAX; i = builder->entries[i].next) {
        PostingBuildEntry* entry = &builder->entries[i];
        if (entry->key == key) return entry;
    }
    return NULL;
}

static int ezdb_postings_builder_count_id(PostingBuilder* builder, uint32_t key, uint32_t id)
{
    uint32_t bucket = posting_bucket_for(key, builder->bucket_count);
    for (uint32_t i = builder->buckets[bucket]; i != UINT32_MAX; i = builder->entries[i].next) {
        PostingBuildEntry* entry = &builder->entries[i];
        if (entry->key == key) {
            if (entry->count && entry->cap == id) return EZDB_OK;
            entry->count += 1u;
            entry->cap = id;
            return EZDB_OK;
        }
    }

    if (ezdb_postings_ensure_capacity((void**)&builder->entries, sizeof(PostingBuildEntry), &builder->entry_cap, builder->entry_count + 1u) != EZDB_OK) {
        return EZDB_ERR_MEMORY;
    }
    PostingBuildEntry* entry = &builder->entries[builder->entry_count];
    memset(entry, 0, sizeof(*entry));
    entry->key = key;
    entry->count = 1u;
    entry->cap = id;
    entry->next = builder->buckets[bucket];
    builder->buckets[bucket] = builder->entry_count;
    builder->entry_count += 1u;
    return EZDB_OK;
}

int ezdb_postings_builder_add_count(PostingBuilder* builder, uint32_t key, uint32_t count)
{
    if (!builder || !builder->buckets || !builder->bucket_count) return EZDB_ERR_ARG;
    if (!count) return EZDB_OK;
    uint32_t bucket = posting_bucket_for(key, builder->bucket_count);
    for (uint32_t i = builder->buckets[bucket]; i != UINT32_MAX; i = builder->entries[i].next) {
        PostingBuildEntry* entry = &builder->entries[i];
        if (entry->key == key) {
            if (UINT32_MAX - entry->count < count) return EZDB_ERR_MEMORY;
            entry->count += count;
            entry->cap = 0;
            return EZDB_OK;
        }
    }

    if (ezdb_postings_ensure_capacity((void**)&builder->entries, sizeof(PostingBuildEntry), &builder->entry_cap, builder->entry_count + 1u) != EZDB_OK) {
        return EZDB_ERR_MEMORY;
    }
    PostingBuildEntry* entry = &builder->entries[builder->entry_count];
    memset(entry, 0, sizeof(*entry));
    entry->key = key;
    entry->count = count;
    entry->next = builder->buckets[bucket];
    builder->buckets[bucket] = builder->entry_count;
    builder->entry_count += 1u;
    return EZDB_OK;
}

int ezdb_postings_builder_prepare_fill(PostingBuilder* builder)
{
    uint64_t total_ids = 0;
    for (uint32_t i = 0; i < builder->entry_count; ++i) {
        builder->entries[i].fill_mode = 0;
        builder->entries[i].fill_bytes = 0;
        total_ids += builder->entries[i].count;
    }
    if (total_ids > (uint64_t)(SIZE_MAX / sizeof(uint32_t))) return EZDB_ERR_MEMORY;

    builder->id_block = (uint32_t*)malloc(sizeof(uint32_t) * (size_t)(total_ids ? total_ids : 1u));
    if (!builder->id_block) return EZDB_ERR_MEMORY;

    uint64_t offset = 0;
    for (uint32_t i = 0; i < builder->entry_count; ++i) {
        builder->entries[i].ids = builder->id_block + offset;
        offset += builder->entries[i].count;
        builder->entries[i].cap = 0;
    }
    return EZDB_OK;
}

int ezdb_postings_builder_remove_id(PostingBuilder* builder, uint32_t key, uint32_t id)
{
    if (!builder || !builder->buckets || !builder->bucket_count) return EZDB_OK;
    PostingBuildEntry* entry = ezdb_postings_builder_find(builder, key);
    if (!entry || !entry->ids || !entry->count) return EZDB_OK;
    uint32_t out = 0;
    for (uint32_t i = 0; i < entry->count; ++i) {
        if (entry->ids[i] != id) entry->ids[out++] = entry->ids[i];
    }
    entry->count = out;
    return EZDB_OK;
}

int ezdb_postings_builder_prepare_fill_adaptive(PostingBuilder* builder, uint32_t universe_count)
{
    uint64_t bytes = 0;
    uint32_t bitset_bytes = (universe_count + 7u) / 8u;
    for (uint32_t i = 0; i < builder->entry_count; ++i) {
        PostingBuildEntry* entry = &builder->entries[i];
        if (entry->count >= universe_count / EZDB_BITSET_DENSITY_DIVISOR && bitset_bytes > 0) {
            entry->fill_mode = EZDB_POSTING_BITSET;
            entry->fill_bytes = bitset_bytes;
            bytes += bitset_bytes;
        } else {
            entry->fill_mode = 0;
            entry->fill_bytes = 0;
            bytes += (uint64_t)entry->count * sizeof(uint32_t);
        }
    }
    if (bytes > (uint64_t)SIZE_MAX) return EZDB_ERR_MEMORY;
    builder->id_block = (uint32_t*)malloc((size_t)(bytes ? bytes : 1u));
    if (!builder->id_block) return EZDB_ERR_MEMORY;

    unsigned char* base = (unsigned char*)builder->id_block;
    uint64_t offset = 0;
    for (uint32_t i = 0; i < builder->entry_count; ++i) {
        PostingBuildEntry* entry = &builder->entries[i];
        entry->ids = (uint32_t*)(base + offset);
        if (entry->fill_mode == EZDB_POSTING_BITSET) {
            memset(entry->ids, 0, entry->fill_bytes);
            offset += entry->fill_bytes;
        } else {
            offset += (uint64_t)entry->count * sizeof(uint32_t);
        }
        entry->cap = 0;
    }
    return EZDB_OK;
}

static int ezdb_postings_builder_fill_id(PostingBuilder* builder, uint32_t key, uint32_t id)
{
    PostingBuildEntry* entry = ezdb_postings_builder_find(builder, key);
    if (!entry) return EZDB_ERR_FORMAT;
    if (entry->fill_mode == EZDB_POSTING_BITSET) {
        unsigned char* bits = (unsigned char*)entry->ids;
        bits[id >> 3u] |= (unsigned char)(1u << (id & 7u));
        return EZDB_OK;
    }
    if (entry->cap && entry->ids[entry->cap - 1u] == id) return EZDB_OK;
    if (entry->cap >= entry->count) return EZDB_ERR_FORMAT;
    entry->ids[entry->cap++] = id;
    return EZDB_OK;
}

static void ezdb_postings_atomic_or_byte(unsigned char* ptr, unsigned char mask)
{
#if defined(_MSC_VER)
    volatile char* target = (volatile char*)ptr;
    char old_value;
    char new_value;
    do {
        old_value = *target;
        new_value = (char)(old_value | (char)mask);
    } while (_InterlockedCompareExchange8(target, new_value, old_value) != old_value);
#else
    *ptr |= mask;
#endif
}

static int ezdb_postings_builder_fill_id_sliced(PostingBuilder* builder, PostingBuilder* slice_builder, uint32_t key, uint32_t id)
{
    PostingBuildEntry* entry = ezdb_postings_builder_find(builder, key);
    if (!entry) return EZDB_ERR_FORMAT;
    if (entry->fill_mode == EZDB_POSTING_BITSET) {
        unsigned char* bits = (unsigned char*)entry->ids;
        ezdb_postings_atomic_or_byte(&bits[id >> 3u], (unsigned char)(1u << (id & 7u)));
        return EZDB_OK;
    }
    PostingBuildEntry* slice = ezdb_postings_builder_find(slice_builder, key);
    if (!slice) return EZDB_ERR_FORMAT;
    if (slice->cap >= slice->fill_bytes || slice->cap >= entry->count) return EZDB_ERR_FORMAT;
    entry->ids[slice->cap++] = id;
    return EZDB_OK;
}

static int ezdb_postings_add_text_grams_ex(PostingBuilder* builder, PostingBuilder* slice_builder, const char* text, uint32_t id, int mode)
{
    uint32_t len = (uint32_t)strlen(text);
    if (!len) return EZDB_OK;
    uint32_t stack_offsets[EZDB_STACK_TOKENS];
    uint32_t stack_lens[EZDB_STACK_TOKENS];
    uint32_t stack_keys[EZDB_STACK_KEYS];
    uint32_t* offsets = stack_offsets;
    uint32_t* lens = stack_lens;
    uint32_t* keys = stack_keys;
    uint32_t token_cap = EZDB_STACK_TOKENS;
    uint32_t key_cap = EZDB_STACK_KEYS;
    uint32_t token_count = 0;
    uint32_t key_count = 0;
    uint32_t pos = 0;
    while (pos < len) {
        int token_len = ezdb_postings_utf8_token_len((const unsigned char*)text + pos, (size_t)(len - pos));
        if (token_count >= token_cap) {
            uint32_t next_cap = token_cap * 2u;
            uint32_t* new_offsets = (uint32_t*)malloc(sizeof(uint32_t) * (size_t)next_cap);
            uint32_t* new_lens = (uint32_t*)malloc(sizeof(uint32_t) * (size_t)next_cap);
            if (!new_offsets || !new_lens) {
                free(new_offsets);
                free(new_lens);
                if (offsets != stack_offsets) free(offsets);
                if (lens != stack_lens) free(lens);
                return EZDB_ERR_MEMORY;
            }
            memcpy(new_offsets, offsets, sizeof(uint32_t) * (size_t)token_count);
            memcpy(new_lens, lens, sizeof(uint32_t) * (size_t)token_count);
            if (offsets != stack_offsets) free(offsets);
            if (lens != stack_lens) free(lens);
            offsets = new_offsets;
            lens = new_lens;
            token_cap = next_cap;
        }
        offsets[token_count] = pos;
        lens[token_count] = (uint32_t)token_len;
        ++token_count;
        pos += (uint32_t)token_len;
    }
    if (token_count * EZDB_MAX_GRAM_TOKENS > key_cap) {
        key_cap = token_count * EZDB_MAX_GRAM_TOKENS;
        keys = (uint32_t*)malloc(sizeof(uint32_t) * (size_t)key_cap);
        if (!keys) {
            if (offsets != stack_offsets) free(offsets);
            if (lens != stack_lens) free(lens);
            return EZDB_ERR_MEMORY;
        }
    }
    for (uint32_t kind = EZDB_GRAM1; kind <= EZDB_GRAM3; ++kind) {
        if (token_count < kind) continue;
        for (uint32_t i = 0; i + kind <= token_count; ++i) {
            uint32_t byte_start = offsets[i];
            uint32_t byte_end = offsets[i + kind - 1u] + lens[i + kind - 1u];
            keys[key_count++] = ezdb_postings_make_gram_key_from_span(text, byte_start, byte_end - byte_start, kind);
        }
    }
    if (offsets != stack_offsets) free(offsets);
    if (lens != stack_lens) free(lens);
    uint32_t seen_stack[EZDB_STACK_KEYS];
    uint32_t* seen_keys = key_count <= EZDB_STACK_KEYS ? seen_stack : (uint32_t*)malloc(sizeof(uint32_t) * (size_t)key_count);
    if (!seen_keys) {
        if (keys != stack_keys) free(keys);
        return EZDB_ERR_MEMORY;
    }
    uint32_t seen_count = 0;
    for (uint32_t i = 0; i < key_count; ++i) {
        int duplicate = 0;
        for (uint32_t j = 0; j < seen_count; ++j) {
            if (seen_keys[j] == keys[i]) {
                duplicate = 1;
                break;
            }
        }
        if (duplicate) continue;
        seen_keys[seen_count++] = keys[i];
        int rc = EZDB_OK;
        if (mode == 1) {
            rc = ezdb_postings_builder_count_id(builder, keys[i], id);
        } else if (mode == 2) {
            rc = ezdb_postings_builder_fill_id(builder, keys[i], id);
        } else if (mode == 3) {
            rc = ezdb_postings_builder_fill_id_sliced(builder, slice_builder, keys[i], id);
        } else {
            rc = ezdb_postings_builder_add(builder, keys[i], id);
        }
        if (rc != EZDB_OK) {
            if (seen_keys != seen_stack) free(seen_keys);
            if (keys != stack_keys) free(keys);
            return rc;
        }
    }
    if (seen_keys != seen_stack) free(seen_keys);
    if (keys != stack_keys) free(keys);
    return EZDB_OK;
}

int ezdb_postings_add_text_grams(PostingBuilder* builder, const char* text, uint32_t id, int mode)
{
    return ezdb_postings_add_text_grams_ex(builder, NULL, text, id, mode);
}

int ezdb_postings_count_text_grams(PostingBuilder* builder, const char* text, uint32_t id)
{
    return ezdb_postings_add_text_grams(builder, text, id, 1);
}

int ezdb_postings_fill_text_grams(PostingBuilder* builder, const char* text, uint32_t id)
{
    return ezdb_postings_add_text_grams(builder, text, id, 2);
}

int ezdb_postings_fill_text_grams_sliced(PostingBuilder* builder, PostingBuilder* slice_builder, const char* text, uint32_t id)
{
    if (!slice_builder) return EZDB_ERR_ARG;
    return ezdb_postings_add_text_grams_ex(builder, slice_builder, text, id, 3);
}

static int ezdb_postings_append_varuint(unsigned char** data, uint32_t* size, uint32_t* cap, uint32_t value)
{
    do {
        unsigned char byte = (unsigned char)(value & 0x7fu);
        value >>= 7u;
        if (value) byte |= 0x80u;
        if (ezdb_postings_ensure_capacity((void**)data, 1, cap, *size + 1u) != EZDB_OK) return EZDB_ERR_MEMORY;
        (*data)[(*size)++] = byte;
    } while (value);
    return EZDB_OK;
}

static uint32_t varuint_size(uint32_t value)
{
    uint32_t count = 0;
    do {
        ++count;
        value >>= 7u;
    } while (value);
    return count;
}

static uint32_t ezdb_postings_estimate_array_size(const uint32_t* ids, uint32_t count)
{
    uint32_t bytes = 0;
    uint32_t prev = 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t delta = i == 0 ? ids[i] : ids[i] - prev;
        bytes += varuint_size(delta);
        prev = ids[i];
    }
    return bytes;
}

static uint32_t ezdb_postings_count_ranges(const uint32_t* ids, uint32_t count)
{
    if (!count) return 0;
    uint32_t ranges = 1;
    for (uint32_t i = 1; i < count; ++i) {
        if (ids[i] != ids[i - 1u] + 1u) ++ranges;
    }
    return ranges;
}

static uint32_t ezdb_postings_estimate_range_size(const uint32_t* ids, uint32_t count)
{
    if (!count) return 0;
    uint32_t bytes = 0;
    uint32_t i = 0;
    uint32_t prev_start = 0;
    uint32_t range_index = 0;
    while (i < count) {
        uint32_t start = ids[i];
        uint32_t end = start;
        while (i + 1u < count && ids[i + 1u] == end + 1u) {
            ++i;
            ++end;
        }
        uint32_t len = end - start + 1u;
        bytes += varuint_size(range_index == 0 ? start : start - prev_start);
        bytes += varuint_size(len);
        prev_start = start;
        ++range_index;
        ++i;
    }
    return bytes;
}

static int encode_array_container(const uint32_t* ids, uint32_t count, unsigned char** out_data, uint32_t* out_size)
{
    unsigned char* data = NULL;
    uint32_t size = 0, cap = 0;
    uint32_t prev = 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t delta = i == 0 ? ids[i] : ids[i] - prev;
        int rc = ezdb_postings_append_varuint(&data, &size, &cap, delta);
        if (rc != EZDB_OK) {
            free(data);
            return rc;
        }
        prev = ids[i];
    }
    *out_data = data;
    *out_size = size;
    return EZDB_OK;
}

static int encode_range_container(const uint32_t* ids, uint32_t count, unsigned char** out_data, uint32_t* out_size)
{
    unsigned char* data = NULL;
    uint32_t size = 0, cap = 0;
    uint32_t i = 0;
    uint32_t prev_start = 0;
    uint32_t range_index = 0;
    while (i < count) {
        uint32_t start = ids[i];
        uint32_t end = start;
        while (i + 1u < count && ids[i + 1u] == end + 1u) {
            ++i;
            ++end;
        }
        int rc = ezdb_postings_append_varuint(&data, &size, &cap, range_index == 0 ? start : start - prev_start);
        if (rc == EZDB_OK) rc = ezdb_postings_append_varuint(&data, &size, &cap, end - start + 1u);
        if (rc != EZDB_OK) {
            free(data);
            return rc;
        }
        prev_start = start;
        ++range_index;
        ++i;
    }
    *out_data = data;
    *out_size = size;
    return EZDB_OK;
}

static int encode_bitset_container(const uint32_t* ids, uint32_t count, uint32_t universe_count, unsigned char** out_data, uint32_t* out_size)
{
    uint32_t byte_count = (universe_count + 7u) / 8u;
    unsigned char* bits = (unsigned char*)calloc(byte_count ? byte_count : 1u, 1);
    if (!bits) return EZDB_ERR_MEMORY;
    for (uint32_t i = 0; i < count; ++i) {
        if (ids[i] < universe_count) bits[ids[i] >> 3u] |= (unsigned char)(1u << (ids[i] & 7u));
    }
    *out_data = bits;
    *out_size = byte_count;
    return EZDB_OK;
}

static int ezdb_postings_encode_container(const uint32_t* ids, uint32_t count, uint32_t universe_count, uint32_t type, unsigned char** out_data, uint32_t* out_size)
{
    if (type == EZDB_POSTING_RANGE) return encode_range_container(ids, count, out_data, out_size);
    if (type == EZDB_POSTING_BITSET) return encode_bitset_container(ids, count, universe_count, out_data, out_size);
    return encode_array_container(ids, count, out_data, out_size);
}

static int ezdb_postings_read_varuint_mem(const unsigned char* data, uint32_t size, uint32_t* pos, uint32_t* out)
{
    uint32_t value = 0;
    uint32_t shift = 0;
    while (*pos < size) {
        unsigned char byte = data[(*pos)++];
        value |= (uint32_t)(byte & 0x7fu) << shift;
        if (!(byte & 0x80u)) {
            *out = value;
            return EZDB_OK;
        }
        shift += 7u;
        if (shift >= 35u) return EZDB_ERR_FORMAT;
    }
    return EZDB_ERR_FORMAT;
}

EzdbDiskIndex* ezdb_postings_find_index(EzdbDiskIndex* index, uint64_t count, uint32_t key)
{
    EzdbDiskIndex search_key;
    search_key.key = key;
    search_key.count = 0;
    search_key.offset = 0;
    return (EzdbDiskIndex*)bsearch(&search_key, index, (size_t)count, sizeof(EzdbDiskIndex), ezdb_postings_index_compare);
}

int ezdb_postings_load(FILE* fp, uint64_t postings_offset, const EzdbDiskIndex* idx, uint32_t** out_ids)
{
    if (!fp || !idx || !out_ids) return EZDB_ERR_ARG;
    *out_ids = NULL;
    uint32_t* ids = (uint32_t*)malloc(sizeof(uint32_t) * (size_t)idx->count);
    if (!ids && idx->count) return EZDB_ERR_MEMORY;
    if (fseek(fp, (long)(postings_offset + idx->offset), SEEK_SET) != 0) {
        free(ids);
        return EZDB_ERR_IO;
    }
    unsigned char* encoded = (unsigned char*)malloc(idx->encoded_size ? idx->encoded_size : 1u);
    if (!encoded) {
        free(ids);
        return EZDB_ERR_MEMORY;
    }
    if (idx->encoded_size && fread(encoded, 1, idx->encoded_size, fp) != idx->encoded_size) {
        free(encoded);
        free(ids);
        return EZDB_ERR_IO;
    }

    uint32_t raw_size = idx->raw_size ? idx->raw_size : idx->encoded_size;
    unsigned char* raw = encoded;
    if (idx->container_type & EZDB_POSTING_COMPRESSED) {
        raw = (unsigned char*)malloc(raw_size ? raw_size : 1u);
        if (!raw) {
            free(encoded);
            free(ids);
            return EZDB_ERR_MEMORY;
        }
        uLongf dest_len = (uLongf)raw_size;
        int zrc = uncompress(raw, &dest_len, encoded, (uLong)idx->encoded_size);
        free(encoded);
        if (zrc != Z_OK || dest_len != raw_size) {
            free(raw);
            free(ids);
            return EZDB_ERR_FORMAT;
        }
    }

    uint32_t container_type = idx->container_type & EZDB_POSTING_TYPE_MASK;
    if (container_type == EZDB_POSTING_ARRAY) {
        uint32_t current = 0;
        uint32_t pos = 0;
        for (uint32_t i = 0; i < idx->count; ++i) {
            uint32_t delta = 0;
            int rc = ezdb_postings_read_varuint_mem(raw, raw_size, &pos, &delta);
            if (rc != EZDB_OK) {
                if (raw != encoded) free(raw);
                else free(encoded);
                free(ids);
                return rc;
            }
            current = i == 0 ? delta : current + delta;
            ids[i] = current;
        }
    } else if (container_type == EZDB_POSTING_RANGE) {
        uint32_t n = 0;
        uint32_t current_start = 0;
        uint32_t pos = 0;
        while (n < idx->count) {
            uint32_t start_delta = 0;
            uint32_t len = 0;
            int rc = ezdb_postings_read_varuint_mem(raw, raw_size, &pos, &start_delta);
            if (rc == EZDB_OK) rc = ezdb_postings_read_varuint_mem(raw, raw_size, &pos, &len);
            if (rc != EZDB_OK) {
                if (raw != encoded) free(raw);
                else free(encoded);
                free(ids);
                return rc;
            }
            current_start = n == 0 ? start_delta : current_start + start_delta;
            for (uint32_t j = 0; j < len && n < idx->count; ++j) ids[n++] = current_start + j;
        }
    } else if (container_type == EZDB_POSTING_BITSET) {
        uint32_t n = 0;
        for (uint32_t byte_i = 0; byte_i < raw_size && n < idx->count; ++byte_i) {
            unsigned char byte = raw[byte_i];
            while (byte && n < idx->count) {
                unsigned bit = 0;
                while (bit < 8u && !(byte & (1u << bit))) ++bit;
                if (bit >= 8u) break;
                ids[n++] = byte_i * 8u + bit;
                byte &= (unsigned char)~(1u << bit);
            }
        }
        if (n != idx->count) {
            if (raw != encoded) free(raw);
            else free(encoded);
            free(ids);
            return EZDB_ERR_FORMAT;
        }
    } else {
        if (raw != encoded) free(raw);
        else free(encoded);
        free(ids);
        return EZDB_ERR_FORMAT;
    }
    if (raw != encoded) free(raw);
    else free(encoded);
    *out_ids = ids;
    return EZDB_OK;
}

int ezdb_postings_load_intersected(FILE* fp,
                                   uint64_t postings_offset,
                                   EzdbDiskIndex* index,
                                   uint64_t index_count,
                                   const uint32_t* keys,
                                   uint32_t key_count,
                                   uint32_t** out_ids,
                                   uint32_t* out_count)
{
    if (!out_ids || !out_count) return EZDB_ERR_ARG;
    *out_ids = NULL;
    *out_count = 0;
    if (!fp || !index || !keys || !key_count) return EZDB_OK;
    EzdbPostingQueryIndex* qis = (EzdbPostingQueryIndex*)malloc(sizeof(EzdbPostingQueryIndex) * (size_t)key_count);
    if (!qis) return EZDB_ERR_MEMORY;
    for (uint32_t i = 0; i < key_count; ++i) {
        EzdbDiskIndex* idx = ezdb_postings_find_index(index, index_count, keys[i]);
        if (!idx) {
            free(qis);
            return EZDB_OK;
        }
        qis[i].idx = idx;
    }
    qsort(qis, key_count, sizeof(EzdbPostingQueryIndex), ezdb_postings_query_index_compare);
    uint32_t* current = NULL;
    uint32_t current_count = 0;
    int rc = EZDB_OK;
    for (uint32_t i = 0; i < key_count && rc == EZDB_OK; ++i) {
        uint32_t* ids = NULL;
        rc = ezdb_postings_load(fp, postings_offset, qis[i].idx, &ids);
        if (rc != EZDB_OK) break;
        if (!current) {
            current = ids;
            current_count = qis[i].idx->count;
        } else {
            uint32_t max_next = current_count < qis[i].idx->count ? current_count : qis[i].idx->count;
            uint32_t* next = (uint32_t*)malloc(sizeof(uint32_t) * (size_t)max_next);
            if (!next && max_next) {
                free(ids);
                rc = EZDB_ERR_MEMORY;
                break;
            }
            uint32_t a = 0;
            uint32_t b = 0;
            uint32_t n = 0;
            while (a < current_count && b < qis[i].idx->count) {
                if (current[a] == ids[b]) {
                    next[n++] = current[a];
                    ++a;
                    ++b;
                } else if (current[a] < ids[b]) {
                    ++a;
                } else {
                    ++b;
                }
            }
            free(current);
            free(ids);
            current = next;
            current_count = n;
            if (!current_count) break;
        }
    }
    free(qis);
    if (rc != EZDB_OK) {
        free(current);
        return rc;
    }
    *out_ids = current;
    *out_count = current_count;
    return EZDB_OK;
}

int ezdb_postings_load_intersected_memory(PostingBuilder* builder,
                                          const uint32_t* keys,
                                          uint32_t key_count,
                                          uint32_t** out_ids,
                                          uint32_t* out_count)
{
    if (!out_ids || !out_count) return EZDB_ERR_ARG;
    *out_ids = NULL;
    *out_count = 0;
    if (!builder || !builder->buckets || !builder->entry_count || !keys || !key_count) return EZDB_OK;
    PostingBuildEntry** entries = (PostingBuildEntry**)malloc(sizeof(PostingBuildEntry*) * (size_t)key_count);
    if (!entries) return EZDB_ERR_MEMORY;
    for (uint32_t i = 0; i < key_count; ++i) {
        PostingBuildEntry* entry = ezdb_postings_builder_find(builder, keys[i]);
        if (!entry || !entry->count) {
            free(entries);
            return EZDB_OK;
        }
        entries[i] = entry;
    }
    for (uint32_t i = 0; i + 1u < key_count; ++i) {
        for (uint32_t j = i + 1u; j < key_count; ++j) {
            if (entries[j]->count < entries[i]->count) {
                PostingBuildEntry* tmp = entries[i];
                entries[i] = entries[j];
                entries[j] = tmp;
            }
        }
    }
    uint32_t* current = (uint32_t*)malloc(sizeof(uint32_t) * (size_t)entries[0]->count);
    if (!current && entries[0]->count) {
        free(entries);
        return EZDB_ERR_MEMORY;
    }
    memcpy(current, entries[0]->ids, sizeof(uint32_t) * (size_t)entries[0]->count);
    uint32_t current_count = entries[0]->count;
    for (uint32_t i = 1; i < key_count && current_count; ++i) {
        uint32_t next_cap = current_count < entries[i]->count ? current_count : entries[i]->count;
        uint32_t* next = (uint32_t*)malloc(sizeof(uint32_t) * (size_t)next_cap);
        if (!next && next_cap) {
            free(current);
            free(entries);
            return EZDB_ERR_MEMORY;
        }
        uint32_t a = 0;
        uint32_t b = 0;
        uint32_t n = 0;
        while (a < current_count && b < entries[i]->count) {
            if (current[a] == entries[i]->ids[b]) {
                next[n++] = current[a];
                ++a;
                ++b;
            } else if (current[a] < entries[i]->ids[b]) {
                ++a;
            } else {
                ++b;
            }
        }
        free(current);
        current = next;
        current_count = n;
    }
    free(entries);
    *out_ids = current;
    *out_count = current_count;
    return EZDB_OK;
}

int ezdb_postings_write(FILE* out,
                   PostingBuilder* builder,
                   uint32_t universe_count,
                   EzdbDiskIndex** out_index,
                   uint32_t* out_index_count,
                   uint64_t* out_written,
                   PostingWriteStats* stats)
{
    *out_index = NULL;
    *out_index_count = 0;
    *out_written = 0;
    if (stats) memset(stats, 0, sizeof(*stats));
    if (!builder->entry_count) return EZDB_OK;

    double stage_start_ms = ezdb_postings_now_ms();
    PostingBuildEntry** sorted = (PostingBuildEntry**)malloc(sizeof(PostingBuildEntry*) * (size_t)builder->entry_count);
    if (!sorted) return EZDB_ERR_MEMORY;
    for (uint32_t i = 0; i < builder->entry_count; ++i) sorted[i] = &builder->entries[i];
    qsort(sorted, builder->entry_count, sizeof(PostingBuildEntry*), posting_entry_key_compare);
    if (stats) stats->sort_ms = ezdb_postings_now_ms() - stage_start_ms;

    EzdbDiskIndex* indexes = NULL;
    uint32_t index_count = 0;
    uint32_t index_cap = 0;
    uint64_t written = 0;
    for (uint32_t entry_i = 0; entry_i < builder->entry_count; ++entry_i) {
        PostingBuildEntry* entry = sorted[entry_i];
        if (!entry->count) continue;

        uint32_t bitset_size = (universe_count + 7u) / 8u;
        uint32_t type = EZDB_POSTING_ARRAY;
        uint32_t encoded_size = 0;
        unsigned char* raw_payload = NULL;
        uint32_t raw_size = 0;
        int rc = EZDB_OK;
        stage_start_ms = ezdb_postings_now_ms();
        if (entry->fill_mode == EZDB_POSTING_BITSET) {
            type = EZDB_POSTING_BITSET;
            encoded_size = bitset_size;
            raw_size = entry->fill_bytes;
            if (stats) stats->bitset_count += 1u;
            if (stats) stats->choose_ms += ezdb_postings_now_ms() - stage_start_ms;
            stage_start_ms = ezdb_postings_now_ms();
            raw_payload = (unsigned char*)malloc(raw_size ? raw_size : 1u);
            if (!raw_payload) {
                free(sorted);
                free(indexes);
                return EZDB_ERR_MEMORY;
            }
            if (raw_size) memcpy(raw_payload, entry->ids, raw_size);
            if (stats) stats->encode_ms += ezdb_postings_now_ms() - stage_start_ms;
        } else {
            uint32_t array_size = ezdb_postings_estimate_array_size(entry->ids, entry->count);
            uint32_t range_count = ezdb_postings_count_ranges(entry->ids, entry->count);
            uint32_t range_size = ezdb_postings_estimate_range_size(entry->ids, entry->count);
            encoded_size = array_size;
            if (range_count <= entry->count / 2u && range_size < encoded_size) {
                type = EZDB_POSTING_RANGE;
                encoded_size = range_size;
            }
            if (entry->count >= universe_count / EZDB_BITSET_DENSITY_DIVISOR && bitset_size < encoded_size) {
                type = EZDB_POSTING_BITSET;
                encoded_size = bitset_size;
            }
            if (stats) {
                if (type == EZDB_POSTING_ARRAY) stats->array_count += 1u;
                else if (type == EZDB_POSTING_RANGE) stats->range_count += 1u;
                else if (type == EZDB_POSTING_BITSET) stats->bitset_count += 1u;
                stats->choose_ms += ezdb_postings_now_ms() - stage_start_ms;
            }
            stage_start_ms = ezdb_postings_now_ms();
            rc = ezdb_postings_encode_container(entry->ids, entry->count, universe_count, type, &raw_payload, &raw_size);
            if (stats) stats->encode_ms += ezdb_postings_now_ms() - stage_start_ms;
        }
        if (rc != EZDB_OK) {
            free(sorted);
            free(indexes);
            return rc;
        }

        unsigned char* payload = NULL;
        uint32_t payload_size = 0;
        int compressed = 0;
        stage_start_ms = ezdb_postings_now_ms();
        rc = maybe_compress_payload(raw_payload, raw_size, &payload, &payload_size, &compressed);
        if (stats) {
            stats->compress_ms += ezdb_postings_now_ms() - stage_start_ms;
            stats->raw_bytes += raw_size;
            stats->encoded_bytes += payload_size;
            if (compressed) stats->compressed_count += 1u;
        }
        free(raw_payload);
        if (rc != EZDB_OK) {
            free(sorted);
            free(indexes);
            return rc;
        }

        uint64_t local_offset = written;
        stage_start_ms = ezdb_postings_now_ms();
        rc = write_bytes(out, payload, payload_size, &written);
        if (stats) stats->fwrite_ms += ezdb_postings_now_ms() - stage_start_ms;
        free(payload);
        if (rc != EZDB_OK) {
            free(sorted);
            free(indexes);
            return rc;
        }

        if (ezdb_postings_ensure_capacity((void**)&indexes, sizeof(EzdbDiskIndex), &index_cap, index_count + 1) != EZDB_OK) {
            free(sorted);
            free(indexes);
            return EZDB_ERR_MEMORY;
        }
        stage_start_ms = ezdb_postings_now_ms();
        indexes[index_count].key = entry->key;
        indexes[index_count].count = entry->count;
        indexes[index_count].container_type = type | (compressed ? EZDB_POSTING_COMPRESSED : 0u);
        indexes[index_count].encoded_size = (uint32_t)(written - local_offset);
        indexes[index_count].raw_size = raw_size;
        indexes[index_count].offset = local_offset;
        ++index_count;
        if (stats) stats->index_meta_ms += ezdb_postings_now_ms() - stage_start_ms;
    }
    free(sorted);
    *out_index = indexes;
    *out_index_count = index_count;
    *out_written = written;
    return EZDB_OK;
}
