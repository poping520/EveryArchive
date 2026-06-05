#include "ezdb_postings.h"

#include <stdlib.h>
#include <string.h>

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

int posting_builder_init(PostingBuilder* builder, uint32_t bucket_count)
{
    if (!builder || !bucket_count) return EZDB_ERR_ARG;
    memset(builder, 0, sizeof(*builder));
    builder->bucket_count = bucket_count;
    builder->buckets = (uint32_t*)malloc(sizeof(uint32_t) * (size_t)bucket_count);
    if (!builder->buckets) return EZDB_ERR_MEMORY;
    for (uint32_t i = 0; i < bucket_count; ++i) builder->buckets[i] = UINT32_MAX;
    return EZDB_OK;
}

void posting_builder_free(PostingBuilder* builder)
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

int posting_builder_add(PostingBuilder* builder, uint32_t key, uint32_t id)
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

PostingBuildEntry* posting_builder_find(PostingBuilder* builder, uint32_t key)
{
    if (!builder || !builder->buckets || !builder->bucket_count) return NULL;
    uint32_t bucket = posting_bucket_for(key, builder->bucket_count);
    for (uint32_t i = builder->buckets[bucket]; i != UINT32_MAX; i = builder->entries[i].next) {
        PostingBuildEntry* entry = &builder->entries[i];
        if (entry->key == key) return entry;
    }
    return NULL;
}

static int posting_builder_count_id(PostingBuilder* builder, uint32_t key, uint32_t id)
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

int posting_builder_prepare_fill(PostingBuilder* builder)
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

int posting_builder_remove_id(PostingBuilder* builder, uint32_t key, uint32_t id)
{
    if (!builder || !builder->buckets || !builder->bucket_count) return EZDB_OK;
    PostingBuildEntry* entry = posting_builder_find(builder, key);
    if (!entry || !entry->ids || !entry->count) return EZDB_OK;
    uint32_t out = 0;
    for (uint32_t i = 0; i < entry->count; ++i) {
        if (entry->ids[i] != id) entry->ids[out++] = entry->ids[i];
    }
    entry->count = out;
    return EZDB_OK;
}

int posting_builder_prepare_fill_adaptive(PostingBuilder* builder, uint32_t universe_count)
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

static int posting_builder_fill_id(PostingBuilder* builder, uint32_t key, uint32_t id)
{
    PostingBuildEntry* entry = posting_builder_find(builder, key);
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

int add_text_grams_to_builder(PostingBuilder* builder, const char* text, uint32_t id, int mode)
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
            rc = posting_builder_count_id(builder, keys[i], id);
        } else if (mode == 2) {
            rc = posting_builder_fill_id(builder, keys[i], id);
        } else {
            rc = posting_builder_add(builder, keys[i], id);
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

int count_text_grams(PostingBuilder* builder, const char* text, uint32_t id)
{
    return add_text_grams_to_builder(builder, text, id, 1);
}

int fill_text_grams(PostingBuilder* builder, const char* text, uint32_t id)
{
    return add_text_grams_to_builder(builder, text, id, 2);
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

uint32_t estimate_array_size(const uint32_t* ids, uint32_t count)
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

uint32_t count_ranges(const uint32_t* ids, uint32_t count)
{
    if (!count) return 0;
    uint32_t ranges = 1;
    for (uint32_t i = 1; i < count; ++i) {
        if (ids[i] != ids[i - 1u] + 1u) ++ranges;
    }
    return ranges;
}

uint32_t estimate_range_size(const uint32_t* ids, uint32_t count)
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

int encode_posting_container(const uint32_t* ids, uint32_t count, uint32_t universe_count, uint32_t type, unsigned char** out_data, uint32_t* out_size)
{
    if (type == EZDB_POSTING_RANGE) return encode_range_container(ids, count, out_data, out_size);
    if (type == EZDB_POSTING_BITSET) return encode_bitset_container(ids, count, universe_count, out_data, out_size);
    return encode_array_container(ids, count, out_data, out_size);
}
