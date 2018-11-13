// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "blkdev.h"
#include "sha.h"
#include "atomic.h"

#define HASHES_PER_BLOCK (FS_BLOCK_SIZE / sizeof(fs_sha256_t))

typedef struct _blkdev
{
    fs_blkdev_t base;
    volatile uint64_t ref_count;
    fs_blkdev_t* next;
    size_t nblks;
    const fs_sha256_t* hashes;
    size_t nhashes;

    /* The number of hash blocks. */
    size_t n_hash_blks;

    /* Keeps track of dirty hash blocks. */
    uint8_t* dirty;
} blkdev_t;

FS_INLINE bool _is_power_of_two(size_t n)
{
    return (n & (n - 1)) == 0;
}

/* Get the index of the left child of the given node in the hash tree. */
FS_INLINE size_t _left_child_index(size_t i)
{
    return (2 * i) + 1;
}

/* Get the index of the right child of the given node in the hash tree. */
FS_INLINE size_t _right_child_index(size_t i)
{
    return (2 * i) + 2;
}

/* Get the index of the parent of the given node in the hash tree. */
FS_INLINE size_t _parent_index(size_t i)
{
    if (i == 0)
        return -1;

    return (i - 1) / 2;
}

FS_INLINE size_t _round_to_multiple(size_t x, size_t m)
{
    return (size_t)((x + (m - 1)) / m * m);
}

FS_INLINE size_t _min(size_t x, size_t y)
{
    return (x < y) ? x : y;
}

FS_INLINE int _hash(fs_sha256_t* hash, const void* data, size_t size)
{
    return fs_sha256(hash, data, size);
}

static int _hash2(
    fs_sha256_t* hash,
    const fs_sha256_t* left,
    const fs_sha256_t* right)
{
    int ret = -1;
    typedef struct _data
    {
        fs_sha256_t left;
        fs_sha256_t right;
    } data_t;
    data_t data;

    memset(hash, 0, sizeof(fs_sha256_t));
    data.left = *left;
    data.right = *right;

    if (_hash(hash, &data, sizeof(data)) != 0)
        goto done;

    ret = 0;

done:
    return ret;
}

static void _set_hash(blkdev_t* dev, size_t i, const fs_sha256_t* hash)
{
    uint32_t hblkno = i / HASHES_PER_BLOCK;

    assert(i < dev->nhashes);
    assert(hblkno < dev->n_hash_blks);

    ((fs_sha256_t*)dev->hashes)[i] = *hash;
    dev->dirty[hblkno] = 1;
}

/* Write the hashes just after the data blocks. */
static int _write_hash_tree(blkdev_t* dev)
{
    int ret = -1;
    const fs_blk_t* p = (const fs_blk_t*)dev->hashes;

    for (size_t i = 0; i < dev->n_hash_blks; i++)
    {
        if (dev->dirty[i])
        {
            size_t offset = dev->nblks;

            if (dev->next->put(dev->next, i + offset, p) != 0)
                goto done;

            dev->dirty[i] = 0;
        }

        p++;
    }

    ret = 0;

done:
    return ret;
}

/* Read the hashes just after the data blocks. */
static int read_hash_tree(blkdev_t* dev)
{
    int ret = -1;
    size_t nbytes = dev->nhashes * sizeof(fs_sha256_t);
    size_t nblks = _round_to_multiple(nbytes, FS_BLOCK_SIZE) / FS_BLOCK_SIZE;
    uint8_t* ptr = (uint8_t*)dev->hashes;
    size_t rem = dev->nhashes * sizeof(fs_sha256_t);

    for (size_t i = 0; i < nblks; i++)
    {
        fs_blk_t blk;
        size_t n = _min(rem, sizeof(blk));
        size_t offset = dev->nblks;

        if (dev->next->get(dev->next, i + offset, &blk) != 0)
            goto done;

        memcpy(ptr, &blk, n);
        ptr += n;
        rem -= n;
    }

    ret = 0;
    goto done;

done:
    return ret;
}

/* Check that the hashes in the hash tree are correct. */
static int _check_hash_tree(blkdev_t* dev)
{
    int ret = -1;

    for (size_t i = 0; i < dev->nblks - 1; i++)
    {
        fs_sha256_t hash;

        if (_hash2(
                &hash,
                &dev->hashes[_left_child_index(i)],
                &dev->hashes[_right_child_index(i)]) != 0)
        {
            goto done;
        }

        if (memcmp(&hash, &dev->hashes[i], sizeof(hash)) != 0)
            goto done;
    }

    ret = 0;

done:
    return ret;
}

static int _check_hash(blkdev_t* dev, uint32_t blkno, const fs_sha256_t* hash)
{
    int ret = -1;
    size_t index = (dev->nblks - 1) + blkno;

    if (index > dev->nhashes)
        goto done;

    if (memcmp(&dev->hashes[index], hash, sizeof(fs_sha256_t)) != 0)
        goto done;

    ret = 0;

done:
    return ret;
}

static int _update_hash_tree(
    blkdev_t* dev,
    uint32_t blkno,
    const fs_sha256_t* hash)
{
    int ret = -1;
    size_t index = (dev->nblks - 1) + blkno;
    size_t parent;

    if (index > dev->nhashes)
        goto done;

    /* Update the leaf hash. */
    _set_hash(dev, index, hash);

    /* Get the index of the parent node. */
    parent = _parent_index(index);

    /* Update hashes of the parent nodes. */
    while (parent != -1)
    {
        fs_sha256_t tmp_hash;

        if (_hash2(
                &tmp_hash,
                &dev->hashes[_left_child_index(parent)],
                &dev->hashes[_right_child_index(parent)]) != 0)
        {
            goto done;
        }

        _set_hash(dev, parent, &tmp_hash);
        parent = _parent_index(parent);
    }

    ret = 0;

done:
    return ret;
}

static int _blkdev_release(fs_blkdev_t* blkdev)
{
    int ret = -1;
    blkdev_t* dev = (blkdev_t*)blkdev;

    if (!dev)
        goto done;

    if (fs_atomic_decrement(&dev->ref_count) == 0)
    {
        if (_write_hash_tree(dev) != 0)
            goto done;

        dev->next->release(dev->next);
        free((fs_sha256_t*)dev->hashes);
        free(dev->dirty);
        free(dev);
    }

    ret = 0;

done:
    return ret;
}

static int _blkdev_get(fs_blkdev_t* blkdev, uint32_t blkno, fs_blk_t* blk)
{
    int ret = -1;
    blkdev_t* dev = (blkdev_t*)blkdev;
    fs_sha256_t hash;

    if (!dev || !blk || blkno > dev->nblks)
        goto done;

    if (dev->next->get(dev->next, blkno, blk) != 0)
        goto done;

    if (_hash(&hash, blk, sizeof(fs_blk_t)) != 0)
        goto done;

    /* Check the hash to make sure the block was not tampered with. */
    if (_check_hash(dev, blkno, &hash) != 0)
    {
        memset(blk, 0, sizeof(fs_blk_t));
        goto done;
    }

    ret = 0;

done:

    return ret;
}

static int _blkdev_put(fs_blkdev_t* blkdev, uint32_t blkno, const fs_blk_t* blk)
{
    int ret = -1;
    blkdev_t* dev = (blkdev_t*)blkdev;
    fs_sha256_t hash;

    if (!dev || !blk || blkno > dev->nblks)
        goto done;

    if (_hash(&hash, blk, sizeof(fs_blk_t)) != 0)
        goto done;

    if (_update_hash_tree(dev, blkno, &hash) != 0)
        goto done;

    if (dev->next->put(dev->next, blkno, blk) != 0)
        goto done;

#if defined(EXTRA_CHECKS)
    if (_check_hash_tree(dev) != 0)
        goto done;
#endif

    ret = 0;

done:

    return ret;
}

static int _blkdev_begin(fs_blkdev_t* d)
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

static int _blkdev_end(fs_blkdev_t* d)
{
    int ret = -1;
    blkdev_t* dev = (blkdev_t*)d;

    if (!dev || !dev->next)
        goto done;

    if (_write_hash_tree(dev) != 0)
        goto done;

    if (dev->next->end(dev->next) != 0)
        goto done;

    ret = 0;

done:

    return ret;
}

static int _blkdev_add_ref(fs_blkdev_t* blkdev)
{
    int ret = -1;
    blkdev_t* dev = (blkdev_t*)blkdev;

    if (!dev)
        goto done;

    fs_atomic_increment(&dev->ref_count);

    ret = 0;

done:
    return ret;
}

int fs_merkle_blkdev_open(
    fs_blkdev_t** blkdev,
    bool initialize,
    size_t nblks,
    fs_blkdev_t* next)
{
    int ret = -1;
    blkdev_t* dev = NULL;
    fs_sha256_t* hashes = NULL;
    uint8_t* dirty = NULL;
    size_t nhashes;

    if (blkdev)
        *blkdev = NULL;

    if (!blkdev || !next)
        goto done;

    /* nblks must be greater than 1 and a power of 2. */
    if (!(nblks > 1 && _is_power_of_two(nblks)))
        goto done;

    /* Calculate the number of nodes (hashes) in the hash tree. */
    nhashes = (nblks * 2) - 1;

    /* Allocate the device structure. */
    if (!(dev = calloc(1, sizeof(blkdev_t))))
        goto done;

    /* Allocate the hash tree (allocate extra memory up to block boundary). */
    {
        size_t size = nhashes * sizeof(fs_sha256_t);

        size = _round_to_multiple(size, FS_BLOCK_SIZE);

        if (!(hashes = calloc(1, size)))
            goto done;
    }

    /* Calculate the number of hash blocks. */
    dev->n_hash_blks =
        _round_to_multiple(nhashes, HASHES_PER_BLOCK) / HASHES_PER_BLOCK;

    /* Allocate the dirty bytes for the hash tree. */
    if (!(dirty = calloc(1, dev->n_hash_blks)))
        goto done;

    dev->base.get = _blkdev_get;
    dev->base.put = _blkdev_put;
    dev->base.begin = _blkdev_begin;
    dev->base.end = _blkdev_end;
    dev->base.add_ref = _blkdev_add_ref;
    dev->base.release = _blkdev_release;
    dev->ref_count = 1;
    dev->next = next;
    dev->nblks = nblks;
    dev->hashes = hashes;
    dev->nhashes = nhashes;
    dev->dirty = dirty;

    /* Initialize the blocks. */
    if (initialize)
    {
        fs_blk_t zero_blk;
        fs_sha256_t zero_hash;

        /* Initialize the zero-filled block. */
        memset(&zero_blk, 0, sizeof(zero_blk));

        /* Compute the hash of the zero-filled block. */
        if (_hash(&zero_hash, &zero_blk, sizeof(zero_blk)) != 0)
            goto done;

        /* Write all the data blocks. */
        for (size_t i = 0; i < nblks; i++)
        {
            if (next->put(next, 0, &zero_blk) != 0)
                goto done;
        }

        /* Initialize the hash tree. */
        {
            /* Initialize the leaf nodes. */
            for (size_t i = nblks - 1; i < nhashes; i++)
                _set_hash(dev, i, &zero_hash);

            /* Initialize the non-leaf nodes in reverse. */
            for (size_t i = 0; i < nblks - 1; i++)
            {
                size_t rindex = (nblks - 1) - i - 1;

                fs_sha256_t tmp_hash;

                if (_hash2(
                        &tmp_hash,
                        &dev->hashes[_left_child_index(rindex)],
                        &dev->hashes[_right_child_index(rindex)]) != 0)
                {
                    goto done;
                }

                _set_hash(dev, rindex, &tmp_hash);
            }
        }

#if defined(EXTRA_CHECKS)
        if (_check_hash_tree(dev) != 0)
            goto done;
#endif

        /* Set all the dirty bits so all hash nodes will be written. */
        memset(dev->dirty, 1, dev->n_hash_blks);

        /* Write the hash tree to the next device. */
        if (_write_hash_tree(dev) != 0)
            goto done;
    }
    else
    {
        /* Read the hashe tree from the next device. */
        if (read_hash_tree(dev) != 0)
            goto done;

        /* Check the hash tree. */
        if (_check_hash_tree(dev) != 0)
            goto done;
    }

    next->add_ref(next);
    *blkdev = &dev->base;
    hashes = NULL;
    dirty = NULL;
    dev = NULL;

    ret = 0;

done:

    if (dev)
        free(dev);

    if (hashes)
        free(hashes);

    if (dirty)
        free(dirty);

    return ret;
}

int fs_merkle_blkdev_get_extra_blocks(size_t nblks, size_t* extra_nblks)
{
    int ret = -1;
    size_t nhashes;

    if (!extra_nblks)
        goto done;

    /* nblks must be greater than 1 and a power of 2. */
    if (!(nblks > 1 && _is_power_of_two(nblks)))
        goto done;

    /* Calculate the number of hash nodes in a Merkle tree. */
    nhashes = (nblks * 2) - 1;

    /* Calculate the number of blocks needed by a Merkle hash tree. */
    *extra_nblks = 
        _round_to_multiple(nhashes, HASHES_PER_BLOCK) / HASHES_PER_BLOCK;

    ret = 0;

done:
    return ret;
}
