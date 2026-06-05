#pragma once

#include <stddef.h>
#include <stdint.h>

typedef enum EzdbQueryNodeType {
    EZDB_QUERY_TERM = 1,
    EZDB_QUERY_WILDCARD = 2,
    EZDB_QUERY_NOT = 3,
    EZDB_QUERY_AND = 4,
    EZDB_QUERY_OR = 5
} EzdbQueryNodeType;

typedef struct EzdbQueryNode {
    EzdbQueryNodeType type;
    char* text;
    size_t text_len;
    struct EzdbQueryNode* left;
    struct EzdbQueryNode* right;
} EzdbQueryNode;

int ezdb_query_is_space(unsigned char ch);
int ezdb_query_contains_ascii_casefold(const char* text, size_t text_len, const char* needle, size_t needle_len);
EzdbQueryNode* ezdb_query_parse(const char* keyword);
void ezdb_query_node_free(EzdbQueryNode* node);
int ezdb_query_match_path(const EzdbQueryNode* node, const char* path, size_t path_len);
int ezdb_query_matches_text(EzdbQueryNode* root, const char* keyword, const char* text, size_t text_len);
int ezdb_query_longest_literal_from_wildcard(const char* text, size_t len, const char** out_text, size_t* out_len);
int ezdb_query_build_candidate_keys(const char* text, size_t len, uint32_t** out_keys, uint32_t* out_count);
