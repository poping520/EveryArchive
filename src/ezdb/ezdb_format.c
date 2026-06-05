#include "ezdb_format.h"

#include <string.h>

int ezdb_format_header_is_current(const EzdbHeader* header)
{
    if (!header) return 0;
    return memcmp(header->magic, EZDB_MAGIC, sizeof(header->magic)) == 0 &&
           header->version == EZDB_VERSION &&
           header->header_size == sizeof(EzdbHeader);
}
