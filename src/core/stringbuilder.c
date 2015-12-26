#include <string.h>
#include "stringbuilder.h"

#define GROWTH_SIZE 250

void sb_init(Strbuilder *builder)
{
    builder->alloced = GROWTH_SIZE;
    builder->buffer = malloc(GROWTH_SIZE + 1);
    builder->buffer[0] = 0;
    builder->len = 0;
}

void sb_append(Strbuilder *builder, const char *text)
{
    size_t tlen = strlen(text);
    if(tlen + builder->len > builder->alloced)
    {
        builder->buffer = realloc(builder->buffer, builder->alloced + GROWTH_SIZE + 1);
        builder->alloced += GROWTH_SIZE;
    }

    memcpy(builder->buffer + builder->len, text, tlen);
    builder->len += tlen;
    builder->buffer[tlen] = 0;
}
