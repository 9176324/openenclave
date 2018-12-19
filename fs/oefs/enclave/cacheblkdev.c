// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include <assert.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "blkdev.h"
#include "common.h"
#include "list.h"

#define TABLE_SIZE 1093
#define MAX_ENTRIES 64
#define MAX_FREE 64

typedef struct _entry entry_t;

typedef struct _entry_list entry_list_t;

struct _entry_list
{
    /* Must have same layout as oefs_list_t */
    entry_t* head;
    entry_t* tail;
    size_t size;
};

struct _entry
{
    /* Must align with first two field of oefs_list_node_t */
    entry_t* prev;
    entry_t* next;

    uint32_t blkno;
    oefs_blk_t blk;
    uint32_t index;
};

typedef struct _blkdev
{
    oefs_blkdev_t base;
    volatile uint64_t ref_count;
    oefs_blkdev_t* next;

    entry_t* table[TABLE_SIZE];
    entry_list_t list;

    entry_t* free;
    size_t free_count;

} blkdev_t;

static entry_t* _new_entry(blkdev_t* dev, uint32_t blkno, const oefs_blk_t* blk)
{
    entry_t* entry;

    if ((entry = dev->free))
    {
        dev->free = dev->free->next;
        dev->free_count--;
    }
    else if (!(entry = malloc(sizeof(entry_t))))
    {
        return NULL;
    }

    entry->prev = NULL;
    entry->next = NULL;
    entry->blkno = blkno;
    oefs_blk_copy(&entry->blk, blk);
    entry->index = 0;

    return entry;
}

static void _free_entry(blkdev_t* dev, entry_t* entry)
{
    if (dev->free_count < MAX_FREE)
    {
        entry->next = dev->free;
        dev->free = entry;
        dev->free_count++;
    }
    else
    {
        free(entry);
    }
}

static void _release_entries(blkdev_t* dev)
{
    for (entry_t* p = dev->list.head; p;)
    {
        entry_t* next = p->next;
        free(p);
        p = next;
    }

    for (entry_t* p = dev->free; p;)
    {
        entry_t* next = p->next;
        free(p);
        p = next;
    }
}

OE_INLINE void _remove_entry(blkdev_t* dev, entry_t* entry)
{
    oefs_list_remove((oefs_list_t*)&dev->list, (oefs_list_node_t*)entry);
}

/* Insert entry at front of the list. */
static void _insert_entry(blkdev_t* dev, entry_t* entry)
{
    oefs_list_insert_front((oefs_list_t*)&dev->list, (oefs_list_node_t*)entry);
}

static entry_t* _get_entry(blkdev_t* dev, uint32_t blkno)
{
    size_t h = blkno % TABLE_SIZE;
    size_t n = SIZE_MAX;
    size_t r = 0;

    for (size_t i = h; i < n && dev->table[i]; i++)
    {
        if (dev->table[i]->blkno == blkno)
            return dev->table[i];

        /* If this was the last entry in the table, then wrap around. */
        if (i + 1 == TABLE_SIZE)
        {
            i = 0;
            n = h;
        }

        r++;
    }

    return NULL;
}

static void _put_entry(blkdev_t* dev, entry_t* entry)
{
    size_t h = entry->blkno % TABLE_SIZE;
    bool found_slot = false;

    /* If reached the maximum entry count, evict oldest entry. */
    if (dev->list.size == MAX_ENTRIES)
    {
        entry_t* p = dev->list.tail;
        assert(p);
        _remove_entry(dev, p);
        dev->table[p->index] = NULL;
        _free_entry(dev, p);
    }

    /* Insert entry at the front of the list. */
    _insert_entry(dev, entry);

    /* Insert entry into the table. */
    for (size_t i = h, n = SIZE_MAX; i < n; i++)
    {
        if (dev->table[i] == NULL)
        {
            dev->table[i] = entry;
            entry->index = i;
            found_slot = true;
            break;
        }

        /* If this was the last entry in the table, then wrap around. */
        if (i + 1 == TABLE_SIZE)
        {
            i = 0;
            n = h;
        }
    }

    assert(found_slot);
}

/* Move entry to the front of the list. */
static void _touch_entry(blkdev_t* dev, entry_t* entry)
{
    _remove_entry(dev, entry);
    _insert_entry(dev, entry);
}

static int _cache_blkdev_release(oefs_blkdev_t* d)
{
    int ret = -1;
    blkdev_t* dev = (blkdev_t*)d;

    if (!dev)
        goto done;

    if (oe_atomic_decrement(&dev->ref_count) == 0)
    {
        dev->next->release(dev->next);
        _release_entries(dev);
        free(dev);
    }

    ret = 0;

done:
    return ret;
}

static int _cache_blkdev_get(oefs_blkdev_t* d, uint32_t blkno, oefs_blk_t* blk)
{
    int ret = -1;
    blkdev_t* dev = (blkdev_t*)d;

    if (!dev || !blk)
        goto done;

#if defined(DISABLE_CACHING)
    {
        if (dev->next->get(dev->next, blkno, data) != 0)
            goto done;
    }
#else
    {
        entry_t* entry;

        if ((entry = _get_entry(dev, blkno)))
        {
            oefs_blk_copy(blk, &entry->blk);
            _touch_entry(dev, entry);
        }
        else
        {
            if (dev->next->get(dev->next, blkno, blk) != 0)
                goto done;

            if (!(entry = _new_entry(dev, blkno, blk)))
                goto done;

            _put_entry(dev, entry);
        }
    }
#endif

    ret = 0;

done:

    return ret;
}

static int _cache_blkdev_put(
    oefs_blkdev_t* d,
    uint32_t blkno,
    const oefs_blk_t* blk)
{
    int ret = -1;
    blkdev_t* dev = (blkdev_t*)d;

    if (!dev || !blk)
        goto done;

#if defined(DISABLE_CACHING)
    {
        if (dev->next->put(dev->next, blkno, data) != 0)
            goto done;
    }
#else
    {
        entry_t* entry;

        if ((entry = _get_entry(dev, blkno)))
        {
            if (memcmp(&entry->blk, blk->u.data, OEFS_BLOCK_SIZE) != 0)
            {
                if (dev->next->put(dev->next, blkno, blk) != 0)
                    goto done;

                oefs_blk_copy(&entry->blk, blk);
            }

            _touch_entry(dev, entry);
        }
        else
        {
            if (dev->next->put(dev->next, blkno, blk) != 0)
                goto done;

            if (!(entry = _new_entry(dev, blkno, blk)))
                goto done;

            _put_entry(dev, entry);
        }
    }
#endif

    ret = 0;

done:

    return ret;
}

static int _cache_blkdev_begin(oefs_blkdev_t* d)
{
    int ret = -1;
    blkdev_t* dev = (blkdev_t*)d;

    if (!dev || !dev->next)
        goto done;

    if (dev->next->begin(dev->next) != 0)
        goto done;

    ret = 0;

done:

    return ret;
}

static int _cache_blkdev_end(oefs_blkdev_t* d)
{
    int ret = -1;
    blkdev_t* dev = (blkdev_t*)d;

    if (!dev || !dev->next)
        goto done;

    if (dev->next->end(dev->next) != 0)
        goto done;

    ret = 0;

done:

    return ret;
}

static int _cache_blkdev_add_ref(oefs_blkdev_t* d)
{
    int ret = -1;
    blkdev_t* dev = (blkdev_t*)d;

    if (!dev)
        goto done;

    oe_atomic_increment(&dev->ref_count);

    ret = 0;

done:
    return ret;
}

int oefs_cache_blkdev_open(oefs_blkdev_t** dev_out, oefs_blkdev_t* next)
{
    int ret = -1;
    blkdev_t* dev = NULL;

    if (dev_out)
        *dev_out = NULL;

    if (!dev_out || !next)
        goto done;

    if (!(dev = calloc(1, sizeof(blkdev_t))))
        goto done;

    dev->base.get = _cache_blkdev_get;
    dev->base.put = _cache_blkdev_put;
    dev->base.begin = _cache_blkdev_begin;
    dev->base.end = _cache_blkdev_end;
    dev->base.add_ref = _cache_blkdev_add_ref;
    dev->base.release = _cache_blkdev_release;
    dev->ref_count = 1;
    dev->next = next;
    next->add_ref(next);

    *dev_out = &dev->base;
    dev = NULL;

    ret = 0;

done:

    if (dev)
        free(dev);

    return ret;
}
