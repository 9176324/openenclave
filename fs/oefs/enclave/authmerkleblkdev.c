// Licensed under the MIT License.

#include <assert.h>
#include <openenclave/enclave.h>
#include <openenclave/internal/hexdump.h>
#include <openenclave/internal/print.h>
#include <pthread.h>
#include <mbedtls/aes.h>
#include <mbedtls/cmac.h>
#include <mbedtls/gcm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "blkdev.h"
#include "common.h"
#include "utils.h"
#include "sha.h"

#define ENABLE_TRACE_GOTO
#include "trace.h"

#define TAGS_PER_BLOCK (OEFS_BLOCK_SIZE / sizeof(tag_t))

#define MAGIC 0xea6a86f99e6a4f83

#define AES_GCM_IV_SIZE 12

#define KEY_BITS 256

/* Block layout: [data blocks] [header block] [hash blocks] */

typedef struct _header_block
{
    /* Magic number: MAGIC */
    uint64_t magic;

    /* The total number of data blocks in the system. */
    uint64_t nblks;

    /* The root hash of the Merkle tree. */
    oefs_sha256_t hash;

    uint8_t reserved[OEFS_BLOCK_SIZE - 48];
}
header_block_t;

OE_STATIC_ASSERT(sizeof(header_block_t) == OEFS_BLOCK_SIZE);

typedef struct _tag
{
    uint8_t data[16];
} tag_t;

typedef struct _tag_block
{
    tag_t tags[TAGS_PER_BLOCK];
}
tag_block_t;

OE_STATIC_ASSERT(sizeof(tag_block_t) == OEFS_BLOCK_SIZE);

typedef struct _blkdev
{
    oefs_blkdev_t base;
    volatile uint64_t ref_count;
    oefs_blkdev_t* next;

    uint8_t key[OEFS_KEY_SIZE];

    header_block_t header_block;

    /* Upper part of Merkle tree (excluding leaf nodes). */
    oefs_sha256_t* merkle;

    /* In-memory copy of the hash blocks. */
    tag_block_t* tag_blocks;
    size_t num_tag_blocks;

    /* The dirty hash blocks. */
    uint8_t* dirty_tag_blocks;

    /* True if any dirty_tag_blocks[] elements are non-zero. */
    bool have_dirty_tag_blocks;

} blkdev_t;

OE_INLINE tag_t* _get_tag_ptr(blkdev_t* dev, size_t blkno)
{
    uint32_t i = blkno / TAGS_PER_BLOCK;
    uint32_t j = blkno % TAGS_PER_BLOCK;
    return &dev->tag_blocks[i].tags[j];
}

static int _generate_initialization_vector(
    const uint8_t key[OEFS_KEY_SIZE],
    uint64_t blkno,
    uint8_t iv[AES_GCM_IV_SIZE])
{
    int ret = -1;
    uint8_t in[16];
    uint8_t out[16];
    oefs_sha256_t khash;
    mbedtls_aes_context aes;

    mbedtls_aes_init(&aes);
    memset(iv, 0x00, AES_GCM_IV_SIZE);

    /* The input buffer contains the block number followed by zeros. */
    memset(in, 0, sizeof(in));
    memcpy(in, &blkno, sizeof(blkno));

    /* Compute the hash of the key. */
    if (oefs_sha256(&khash, key, OEFS_KEY_SIZE) != 0)
        goto done;

    /* Use the hash of the key as the key. */
    if (mbedtls_aes_setkey_enc(&aes, khash.data, sizeof(khash) * 8) != 0)
        goto done;

    /* Encrypt the buffer with the hash of the key, yielding the IV. */
    if (mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, in, out) != 0)
        goto done;

    /* Use the first 12 bytes of the 16-byte buffer. */

    memcpy(iv, out, AES_GCM_IV_SIZE);

    ret = 0;

done:

    mbedtls_aes_free(&aes);
    return ret;
}

static int _encrypt(
    const uint8_t key[OEFS_KEY_SIZE],
    uint32_t blkno,
    const uint8_t in[OEFS_BLOCK_SIZE],
    uint8_t out[OEFS_BLOCK_SIZE],
    tag_t* tag)
{
    int rc = -1;
    mbedtls_gcm_context gcm;
    uint8_t iv[AES_GCM_IV_SIZE];

    mbedtls_gcm_init(&gcm);

    memset(tag, 0, sizeof(tag_t));

    if (mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, KEY_BITS) != 0)
        goto done;

    if (_generate_initialization_vector(key, blkno, iv) != 0)
        goto done;

    if (mbedtls_gcm_crypt_and_tag(
            &gcm,
            MBEDTLS_GCM_ENCRYPT,
            OEFS_BLOCK_SIZE,
            iv,
            sizeof(iv),
            NULL,
            0,
            in,
            out,
            sizeof(tag_t),
            (unsigned char*)tag) != 0)
    {
        goto done;
    }

    rc = 0;

done:

    mbedtls_gcm_free(&gcm);

    return rc;
}

static int _decrypt(
    const uint8_t key[OEFS_KEY_SIZE],
    uint32_t blkno,
    const tag_t* tag,
    const uint8_t in[OEFS_BLOCK_SIZE],
    uint8_t out[OEFS_BLOCK_SIZE])
{
    int rc = -1;
    uint8_t iv[AES_GCM_IV_SIZE];
    mbedtls_gcm_context gcm;

    mbedtls_gcm_init(&gcm);

    if (mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, KEY_BITS) != 0)
        goto done;

    if (_generate_initialization_vector(key, blkno, iv) != 0)
        goto done;

    if (mbedtls_gcm_auth_decrypt(
            &gcm,
            OEFS_BLOCK_SIZE,
            iv,
            sizeof(iv),
            NULL,
            0,
            (const unsigned char*)tag,
            sizeof(tag_t),
            in,
            out) != 0)
    {
        goto done;
    }

    rc = 0;

done:

    mbedtls_gcm_free(&gcm);

    return rc;
}

/* Get the index of the left child of the given node in the hash tree. */
OE_INLINE size_t _left_child_index(size_t i)
{
    return (2 * i) + 1;
}

/* Get the index of the right child of the given node in the hash tree. */
OE_INLINE size_t _right_child_index(size_t i)
{
    return (2 * i) + 2;
}

/* Get the index of the parent of the given node in the hash tree. */
OE_INLINE size_t _parent_index(size_t i)
{
    if (i == 0)
        return -1;

    return (i - 1) / 2;
}

OE_INLINE oefs_sha256_t _left_child(blkdev_t* dev, size_t i)
{
    size_t index = _left_child_index(i);
    size_t merkle_size = dev->header_block.nblks - 1;

    if (index < merkle_size)
    {
        return dev->merkle[index];
    }
    else
    {
        oefs_sha256_t hash;
        const tag_t* tags = (tag_t*)dev->tag_blocks;
        memcpy(&hash, &tags[index - merkle_size], sizeof(tag_t));
        memset((uint8_t*)&hash + sizeof(tag_t), 0, sizeof(tag_t));
        return hash;
    }
}

OE_INLINE oefs_sha256_t _right_child(blkdev_t* dev, size_t i)
{
    size_t index = _right_child_index(i);
    size_t merkle_size = dev->header_block.nblks - 1;

    if (index < merkle_size)
    {
        return dev->merkle[index];
    }
    else
    {
        oefs_sha256_t hash;
        const tag_t* tags = (tag_t*)dev->tag_blocks;
        memcpy(&hash, &tags[index - merkle_size], sizeof(tag_t));
        memset((uint8_t*)&hash + sizeof(tag_t), 0, sizeof(tag_t));
        return hash;
    }
}

static int _hash2(
    oefs_sha256_t* hash,
    const oefs_sha256_t* left,
    const oefs_sha256_t* right)
{
    int ret = -1;
    typedef struct _data
    {
        oefs_sha256_t left;
        oefs_sha256_t right;
    } data_t;
    data_t data;

    data.left = *left;
    data.right = *right;

    if (oefs_sha256(hash, &data, sizeof(data)) != 0)
        GOTO(done);

    ret = 0;

done:
    return ret;
}

static void _set_tag(blkdev_t* dev, size_t blkno, const tag_t* tag)
{
    uint32_t i = blkno / TAGS_PER_BLOCK;
    uint32_t j = blkno % TAGS_PER_BLOCK;

    oe_assert(blkno < dev->header_block.nblks);
    oe_assert(i < dev->num_tag_blocks);

    dev->tag_blocks[i].tags[j] = *tag;
    dev->dirty_tag_blocks[i] = 1;
    dev->have_dirty_tag_blocks = true;
}

static int _load_header_block(blkdev_t* dev)
{
    int ret = -1;
    size_t blkno = dev->header_block.nblks;

    if (dev->next->get(dev->next, blkno, (oefs_blk_t*)&dev->header_block) != 0)
        GOTO(done);

    return 0;

done:
    return ret;
}

static int _flush_header_block(blkdev_t* dev)
{
    int ret = -1;
    size_t blkno = dev->header_block.nblks;

    if (dev->next->put(dev->next, blkno, (oefs_blk_t*)&dev->header_block) != 0)
        GOTO(done);

    return 0;

done:
    return ret;
}

static int _flush_merkle(blkdev_t* dev)
{
    int ret = -1;
    size_t blkno;

    if (dev->have_dirty_tag_blocks)
    {
        if (_flush_header_block(dev) != 0)
            GOTO(done);

        /* Calculate the block number of the first hash block. */
        blkno = dev->header_block.nblks + 1;

        /* Flush the dirty hash blocks. */
        for (size_t i = 0; i < dev->num_tag_blocks; i++)
        {
            if (dev->dirty_tag_blocks[i])
            {
                oefs_blk_t* blk = (oefs_blk_t*)&dev->tag_blocks[i];

                if (dev->next->put(dev->next, blkno + i, blk) != 0)
                    GOTO(done);

                dev->dirty_tag_blocks[i] = 0;
            }
        }

        dev->have_dirty_tag_blocks = false;
    }

    ret = 0;

done:
    return ret;
}

static int _initialize_tag_blocks(blkdev_t* dev)
{
    int ret = -1;
    oefs_blk_t zero_blk;
    size_t blkno = 0;

    memset(&zero_blk, 0, sizeof(oefs_blk_t));

    for (size_t i = 0; i < dev->num_tag_blocks; i++)
    {
        oefs_blk_t encrypted;

        for (size_t j = 0; j < TAGS_PER_BLOCK; j++)
        {
            tag_t tag;

            /* Encrypt the zero-block */
            if (_encrypt(
                dev->key, 
                blkno, 
                (const uint8_t*)&zero_blk, 
                (uint8_t*)&encrypted, 
                &tag) != 0)
            {
                goto done;
            }

            dev->tag_blocks[i].tags[j] = tag;
            blkno++;
        }
    }

    ret = 0;

done:
    return ret;
}

static int _compute_upper_hash_tree(blkdev_t* dev)
{
    int ret = -1;
    size_t merkle_size = dev->header_block.nblks - 1;

    /* Initialize the non-leaf nodes in reverse. */
    for (size_t i = 0; i < merkle_size; i++)
    {
        size_t index = merkle_size - i - 1;
        oefs_sha256_t left = _left_child(dev, index);
        oefs_sha256_t right = _right_child(dev, index);
        oefs_sha256_t hash;

        if (_hash2(&hash, &left, &right) != 0)
            GOTO(done);

        dev->merkle[index] = hash;
    }

    ret = 0;

done:
    return ret;
}

static int _load_merkle(blkdev_t* dev)
{
    int ret = -1;
    size_t blkno;
    oefs_sha256_t* merkle = NULL;
    tag_block_t* tag_blocks = NULL;
    uint8_t* dirty_tag_blocks = NULL;

    /* Load the header block. */
    {
        if (_load_header_block(dev) != 0)
            GOTO(done);

        if (dev->header_block.magic != MAGIC)
            GOTO(done);
    }

    /* Allocate the merkle[] array. */
    {
        size_t merkle_size = dev->header_block.nblks - 1;
        size_t alloc_size = merkle_size * sizeof(oefs_sha256_t);

        if (!(merkle = malloc(alloc_size)))
            GOTO(done);

        dev->merkle = merkle;
    }

    /* Calculate dev->num_tag_blocks */
    dev->num_tag_blocks = oefs_round_to_multiple(
        dev->header_block.nblks, TAGS_PER_BLOCK) / TAGS_PER_BLOCK;

    /* Allocate the dev->tag_blocks[] array. */
    {
        size_t alloc_size = dev->num_tag_blocks * sizeof(tag_block_t);

        if (!(tag_blocks = malloc(alloc_size)))
            GOTO(done);

        dev->tag_blocks = tag_blocks;
    }

    /* Allocate the dev->dirty_tag_blocks[] array. */
    {
        size_t alloc_size = dev->num_tag_blocks * sizeof(uint8_t);

        if (!(dirty_tag_blocks = calloc(1, alloc_size)))
            GOTO(done);

        dev->dirty_tag_blocks = dirty_tag_blocks;
    }

    /* Calculate the block number of the first hash block. */
    blkno = dev->header_block.nblks + 1;

    /* Load each of the hash blocks. */
    for (size_t i = 0; i < dev->num_tag_blocks; i++)
    {
        tag_block_t* tag_block = &dev->tag_blocks[i];

        if (dev->next->get(dev->next, i + blkno, (oefs_blk_t*)tag_block) != 0)
            GOTO(done);
    }

    /* Compute the hash tree. */
    if (_compute_upper_hash_tree(dev) != 0)
        GOTO(done);

    /* Fail if the computed root hash is wrong. */
    if (!oefs_sha256_eq(&dev->header_block.hash, &dev->merkle[0]))
        GOTO(done);

    merkle = NULL;
    tag_blocks = NULL;
    dirty_tag_blocks = NULL;

    ret = 0;

done:

    if (merkle)
        free(merkle);

    if (tag_blocks)
        free(tag_blocks);

    if (dirty_tag_blocks)
        free(dirty_tag_blocks);

    return ret;
}

static int _init_merkle(blkdev_t* dev, size_t nblks)
{
    int ret = -1;
    oefs_sha256_t* merkle = NULL;
    tag_block_t* tag_blocks = NULL;
    uint8_t* dirty_tag_blocks = NULL;

    /* Initialize the header block. */
    {
        memset(&dev->header_block, 0, sizeof(dev->header_block));
        dev->header_block.magic = MAGIC;
        dev->header_block.nblks = nblks;
    }

    /* Allocate the merkle[] array. */
    {
        size_t merkle_size = dev->header_block.nblks - 1;
        size_t alloc_size = merkle_size * sizeof(oefs_sha256_t);

        if (!(merkle = malloc(alloc_size)))
            GOTO(done);

        dev->merkle = merkle;
    }

    /* Calculate dev->num_tag_blocks */
    dev->num_tag_blocks = oefs_round_to_multiple(
        dev->header_block.nblks, TAGS_PER_BLOCK) / TAGS_PER_BLOCK;

    /* Allocate the dev->tag_blocks[] array. */
    {
        size_t alloc_size = dev->num_tag_blocks * sizeof(tag_block_t);

        if (!(tag_blocks = malloc(alloc_size)))
            GOTO(done);

        dev->tag_blocks = tag_blocks;
    }

    /* Allocate the dev->dirty_tag_blocks[] array. */
    {
        size_t alloc_size = dev->num_tag_blocks * sizeof(uint8_t);

        if (!(dirty_tag_blocks = calloc(1, alloc_size)))
            GOTO(done);

        dev->dirty_tag_blocks = dirty_tag_blocks;

        /* Set all hash blocks to dirty. */
        memset(dev->dirty_tag_blocks, 1, alloc_size);
        dev->have_dirty_tag_blocks = true;
    }

    /* Initialize the hash blocks. */
    if (_initialize_tag_blocks(dev) != 0)
        GOTO(done);

    /* Compute the upper hash tree. */
    if (_compute_upper_hash_tree(dev) != 0)
        GOTO(done);

    /* Update the master hash in the header. */
    dev->header_block.hash = dev->merkle[0];

    /* Flush the header and hash blocks to disk. */
    if (_flush_merkle(dev) != 0)
        GOTO(done);

    merkle = NULL;
    tag_blocks = NULL;
    dirty_tag_blocks = NULL;

    ret = 0;

done:

    if (merkle)
        free(merkle);

    if (tag_blocks)
        free(tag_blocks);

    if (dirty_tag_blocks)
        free(dirty_tag_blocks);

    return ret;
}

static int _update_hash_tree(blkdev_t* dev, uint32_t blkno, const tag_t* tag)
{
    int ret = -1;
    size_t merkle_size = dev->header_block.nblks - 1;
    size_t index = merkle_size + blkno;
    size_t parent;

    /* Update the leaf hash. */
    _set_tag(dev, blkno, tag);

    /* Get the index of the parent node. */
    parent = _parent_index(index);

    /* Update hashes of the parent nodes. */
    while (parent != -1)
    {
        oefs_sha256_t left = _left_child(dev, parent);
        oefs_sha256_t right = _right_child(dev, parent);
        oefs_sha256_t hash;

        if (_hash2(&hash, &left, &right) != 0)
            GOTO(done);

        parent = _parent_index(parent);
    }

    /* Update the root hash in the header. */
    dev->header_block.hash = dev->merkle[0];

    ret = 0;

done:
    return ret;
}

static int _auth_merkle_blkdev_release(oefs_blkdev_t* blkdev)
{
    int ret = -1;
    blkdev_t* dev = (blkdev_t*)blkdev;

    if (!dev)
        GOTO(done);

    if (oe_atomic_decrement(&dev->ref_count) == 0)
    {
        if (_flush_merkle(dev) != 0)
            GOTO(done);

        dev->next->release(dev->next);
        free(dev->tag_blocks);
        free(dev->dirty_tag_blocks);
        free(dev);
    }

    ret = 0;

done:
    return ret;
}

static int _auth_merkle_blkdev_get(
    oefs_blkdev_t* blkdev,
    uint32_t blkno,
    oefs_blk_t* blk)
{
    int ret = -1;
    blkdev_t* dev = (blkdev_t*)blkdev;
    oefs_blk_t encrypted;

    if (!dev || !blk || blkno > dev->header_block.nblks)
        GOTO(done);

    if (dev->next->get(dev->next, blkno, &encrypted) != 0)
        GOTO(done);

    /* Decrypt the block */
    if (_decrypt(
            dev->key,
            blkno,
            _get_tag_ptr(dev, blkno),
            (const uint8_t*)&encrypted,
            (uint8_t*)blk) != 0)
    {
        goto done;
    }

    ret = 0;

done:

    return ret;
}

static int _auth_merkle_blkdev_put(
    oefs_blkdev_t* blkdev,
    uint32_t blkno,
    const oefs_blk_t* blk)
{
    int ret = -1;
    blkdev_t* dev = (blkdev_t*)blkdev;
    oefs_blk_t encrypted;
    tag_t tag;

    oe_assert(blkno < dev->header_block.nblks);

    if (!dev || !blk || blkno >= dev->header_block.nblks)
        GOTO(done);

    if (_encrypt(
        dev->key, blkno, (const uint8_t*)blk, (uint8_t*)&encrypted, &tag) != 0)
    {
        goto done;
    }

    if (_update_hash_tree(dev, blkno, &tag) != 0)
        GOTO(done);

    if (dev->next->put(dev->next, blkno, &encrypted) != 0)
        GOTO(done);

    ret = 0;

done:

    return ret;
}

static int _auth_merkle_blkdev_begin(oefs_blkdev_t* d)
{
    int ret = -1;
    blkdev_t* dev = (blkdev_t*)d;

    if (!dev || !dev->next)
        GOTO(done);

    if (dev->next->begin(dev->next) != 0)
        GOTO(done);

    ret = 0;

done:

    return ret;
}

static int _auth_merkle_blkdev_end(oefs_blkdev_t* d)
{
    int ret = -1;
    blkdev_t* dev = (blkdev_t*)d;

    if (!dev || !dev->next)
        GOTO(done);

    if (_flush_merkle(dev) != 0)
            GOTO(done);

    if (dev->next->end(dev->next) != 0)
        GOTO(done);

    ret = 0;

done:

    return ret;
}

static int _auth_merkle_blkdev_add_ref(oefs_blkdev_t* blkdev)
{
    int ret = -1;
    blkdev_t* dev = (blkdev_t*)blkdev;

    if (!dev)
        GOTO(done);

    oe_atomic_increment(&dev->ref_count);

    ret = 0;

done:
    return ret;
}

int oefs_auth_merkle_blkdev_open(
    oefs_blkdev_t** blkdev,
    bool initialize,
    size_t nblks,
    const uint8_t key[OEFS_KEY_SIZE],
    oefs_blkdev_t* next)
{
    int ret = -1;
    blkdev_t* dev = NULL;

    if (blkdev)
        *blkdev = NULL;

    if (!blkdev || !next || !key)
        GOTO(done);

    /* nblks must be greater than 1 and a power of 2. */
    if (!(nblks > 1 && oefs_is_pow_of_2(nblks)))
        GOTO(done);

    /* Allocate the device structure. */
    if (!(dev = calloc(1, sizeof(blkdev_t))))
        GOTO(done);

    dev->base.get = _auth_merkle_blkdev_get;
    dev->base.put = _auth_merkle_blkdev_put;
    dev->base.begin = _auth_merkle_blkdev_begin;
    dev->base.end = _auth_merkle_blkdev_end;
    dev->base.add_ref = _auth_merkle_blkdev_add_ref;
    dev->base.release = _auth_merkle_blkdev_release;
    dev->ref_count = 1;
    dev->next = next;
    memcpy(dev->key, key, OEFS_KEY_SIZE);

    /* Either initialize or load the hash list. */
    if (initialize)
    {
        if (_init_merkle(dev, nblks) != 0)
            GOTO(done);
    }
    else
    {
        if (_load_merkle(dev) != 0)
            GOTO(done);
    }

    next->add_ref(next);
    *blkdev = &dev->base;
    dev = NULL;

    ret = 0;

done:

    if (dev)
        free(dev);

    return ret;
}

int oefs_auth_merkle_blkdev_get_extra_blocks(size_t nblks, size_t* extra_nblks)
{
    int ret = -1;
    size_t hblks;

    if (extra_nblks)
        *extra_nblks = 0;

    if (!extra_nblks)
        GOTO(done);

    hblks = oefs_round_to_multiple(nblks, TAGS_PER_BLOCK) / TAGS_PER_BLOCK;

    *extra_nblks = 1 + hblks;

    ret = 0;

done:
    return ret;
}
