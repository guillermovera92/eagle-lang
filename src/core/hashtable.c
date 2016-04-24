/*
 * Copyright (c) 2015-2016 Sam Horlbeck Olsen
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include <stdlib.h>
#include <string.h>
#include "hashtable.h"

// If we have an equals function, use it; otherwise use pointer equality
#define EQU_CHECK(a, b, f) (f ? (f(a, b)) : (!strcmp(a, b)))
#define NO_RESIZE

struct HSTNode_s {
    long hash;
    void *key;
    void *val;
    struct HSTNode_s *next;
};

long hst_djb2(void *val, void *data)
{
    char *str = (char *)val;
    unsigned long hash = 5381;
    char c;

    while((c = *str++))
        hash = ((hash << 5) + hash) + c;

    return hash;
}

Hashtable hst_create()
{
    HSTNode **buckets = calloc(8, sizeof(HSTNode *));

    Hashtable ht = {0, 8, 0, buckets};

    return ht;
}

HSTNode *hst_make_node(long hash, void *key, void *val, char dup)
{
    HSTNode *n = malloc(sizeof(HSTNode));

    if(dup)
    {
        char *k = (char *)key;
        n->key = malloc(strlen(k) + 1);
        strcpy(n->key, k);
    }
    else
        n->key = key;
    n->hash = hash;
    n->val = val;
    n->next = NULL;
    return n;
}

void hst_resize(Hashtable *ht)
{
    int ns = ht->size > 1000 ? ht->size + 1000 : ht->size * 2;
    HSTNode **buckets = calloc(ns, sizeof(HSTNode *));

    int i;
    HSTNode *n;
    HSTNode *p;
    for(i = 0; i < ht->size; i++)
    {
        for(n = ht->buckets[i]; n;)
        {
            p = n->next;
            int mod = (unsigned long)n->hash % ns;
            if(!buckets[mod])
            {
                buckets[mod] = n;
                n->next = NULL;
                n = p;
                continue;
            }

            HSTNode *x = buckets[mod];
            for(; x->next; x = x->next);
            x->next = n;
            n->next = NULL;

            n = p;
        }
    }

    free(ht->buckets);
    ht->buckets = buckets;
    ht->size = ns;
}

void hst_put(Hashtable *ht, void *key, void *val, hst_hash_function hashfunc, hst_equa_function equfunc)
{
#ifndef NO_RESIZE
    if(ht->count + 1 > ht->size * (0.66))
        hst_resize(ht);
#endif

    if(!hashfunc)
        hashfunc = hst_djb2;

    long hash = hashfunc(key, NULL);
    unsigned long mod = ((unsigned long)hash) % ht->size;

    if(!ht->buckets[mod])
    {
        ht->buckets[mod] = hst_make_node(hash, key, val, ht->duplicate_keys);
        ht->count++;
        return;
    }

    HSTNode *last;
    HSTNode *prev = NULL;
    for(last = ht->buckets[mod]; last; last = last->next)
    {
        if(EQU_CHECK(last->key, key, equfunc))
        {
            last->val = val;
            return;
        }

        prev = last;
    }

    ht->count++;
    prev->next = hst_make_node(hash, key, val, ht->duplicate_keys);
}

void *hst_get(Hashtable *ht, void *key, hst_hash_function hashfunc, hst_equa_function equfunc)
{
    if(!hashfunc)
        hashfunc = hst_djb2;

    long hash = hashfunc(key, NULL);
    long mod = (unsigned long)hash % ht->size;

    HSTNode *n = ht->buckets[mod];

    for(; n; n = n->next) if(EQU_CHECK(n->key, key, equfunc)) return n->val;

    return NULL;
}

char *hst_retrieve_duped_key(Hashtable *ht, char *key)
{
    if(!ht->duplicate_keys)
        return NULL;

    long hash = hst_djb2(key, NULL);
    long mod = (unsigned long)hash % ht->size;

    HSTNode *n = ht->buckets[mod];

    for(; n; n = n->next) if(!strcmp(key, n->key)) return n->key;

    return NULL;
}

void hst_add_all_from(Hashtable *ht, Hashtable *ot, hst_hash_function hashfunc, hst_equa_function equfunc)
{
    if(!hashfunc)
        hashfunc = hst_djb2;

    int i;
    for(i = 0; i < ot->size; i++)
    {
        HSTNode *n;
        for(n = ot->buckets[i]; n; n = n->next)
            hst_put(ht, n->key, n->val, hashfunc, equfunc);
    }
}

int hst_contains_key(Hashtable *ht, void *key, hst_hash_function hashfunc, hst_equa_function equfunc)
{
    if(!hashfunc)
        hashfunc = hst_djb2;

    long hash = hashfunc(key, NULL);
    long mod = (unsigned long)hash % ht->size;

    HSTNode *n = ht->buckets[mod];

    for(; n; n = n->next) if(EQU_CHECK(n->key, key, equfunc)) return 1;

    return 0;
}

int hst_contains_value(Hashtable *ht, void *val, hst_equa_function equfunc)
{
    int i;
    HSTNode *n = NULL;
    for(i = 0; i < ht->size; i++)
        for(n = ht->buckets[i]; n; n = n->next)
            if(EQU_CHECK(n->val, val, equfunc))
                return 1;

    return 0;
}

void *hst_remove_key(Hashtable *ht, void *key, hst_hash_function hashfunc, hst_equa_function equfunc)
{
    void *ret = NULL;

    if(!hashfunc)
        hashfunc = hst_djb2;

    long hash = hashfunc(key, NULL);
    long mod = (unsigned long)hash % ht->size;

    HSTNode *n = ht->buckets[mod];
    HSTNode *p = NULL;

    for(; n; n = n->next)
    {
        if(EQU_CHECK(n->key, key, equfunc))
        {
            ret = n->val;
            if(p)
                p->next = n->next;
            else
                ht->buckets[mod] = n->next;

            if(ht->duplicate_keys)
                free(n->key);
            free(n);
            ht->count--;
            break;
        }

        p = n;
    }

    return ret;
}

void hst_remove_val(Hashtable *ht, void *val, hst_equa_function equfunc)
{
    int i;
    HSTNode *n = NULL;
    for(i = 0; i < ht->size; i++)
    {
        HSTNode *p = NULL;
        for(n = ht->buckets[i]; n; n = n->next)
        {
            if(EQU_CHECK(n->val, val, equfunc))
            {
                if(p)
                    p->next = n->next;
                else
                    ht->buckets[i] = n->next;

                if(ht->duplicate_keys)
                    free(n->key);
                free(n);
                ht->count--;
            }

            p = n;
        }
    }
}

void hst_free(Hashtable *ht)
{
    int i;
    for(i = 0; i < ht->size; i++)
    {
        HSTNode *n = ht->buckets[i];
        while(n)
        {
            HSTNode *next = n->next;
            if(ht->duplicate_keys)
                free(n->key);
            free(n);
            n = next;
        }
    }

    free(ht->buckets);
}

void hst_for_each(Hashtable *ht, hst_each_function func, void *data)
{
    int i;
    HSTNode *n;
    for(i = 0; i < ht->size; i++)
        for(n = ht->buckets[i]; n; n = n->next)
            func(n->key, n->val, data);
}
