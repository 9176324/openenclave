// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include <mbedtls/aes.h>
#include <mbedtls/sha256.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "blockdev.h"
#include "trace.h"

#define SHA256_SIZE 32

#define IV_SIZE 16

typedef struct _block_dev
{
    fs_block_dev_t base;
    size_t ref_count;
    pthread_spinlock_t lock;
    uint8_t key[FS_KEY_SIZE];
    mbedtls_aes_context enc_aes;
    mbedtls_aes_context dec_aes;
    fs_block_dev_t* next;
} block_dev_t;

static int _compute_sha256(
    const void* data,
    size_t size,
    uint8_t hash[SHA256_SIZE])
{
    int ret = -1;
    mbedtls_sha256_context ctx;

    mbedtls_sha256_init(&ctx);

    if (mbedtls_sha256_starts_ret(&ctx, 0) != 0)
        goto done;

    if (mbedtls_sha256_update_ret(&ctx, data, size) != 0)
        goto done;

    if (mbedtls_sha256_finish_ret(&ctx, hash) != 0)
        goto done;

    ret = 0;

done:

    mbedtls_sha256_free(&ctx);
    return ret;
}

static int _generate_initialization_vector(
    const uint8_t key[FS_KEY_SIZE],
    uint64_t blkno,
    uint8_t iv[IV_SIZE])
{
    int ret = -1;
    uint8_t buf[IV_SIZE];
    uint8_t key_hash[SHA256_SIZE];
    mbedtls_aes_context aes;

    mbedtls_aes_init(&aes);
    memset(iv, 0, sizeof(IV_SIZE));

    /* The input buffer contains the block number followed by zeros. */
    memset(buf, 0, sizeof(buf));
    memcpy(buf, &blkno, sizeof(blkno));

    /* Compute the hash of the key. */
    if (_compute_sha256(key, FS_KEY_SIZE, key_hash) != 0)
        goto done;

    /* Create a SHA-256 hash of the key. */
    if (mbedtls_aes_setkey_enc(&aes, key_hash, sizeof(key_hash) * 8) != 0)
        goto done;

    /* Encrypt the buffer with the hash of the key, yielding the IV. */
    if (mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, buf, iv) != 0)
        goto done;

    ret = 0;

done:

    mbedtls_aes_free(&aes);
    return ret;
}

static int _crypt(
    mbedtls_aes_context* aes,
    int mode, /* MBEDTLS_AES_ENCRYPT or MBEDTLS_AES_DECRYPT */
    const uint8_t key[FS_KEY_SIZE],
    uint32_t blkno,
    const uint8_t in[FS_BLOCK_SIZE],
    uint8_t out[FS_BLOCK_SIZE])
{
    int rc = -1;
    uint8_t iv[IV_SIZE];

    /* Generate an initialization vector for this block number. */
    if (_generate_initialization_vector(key, blkno, iv) != 0)
        goto done;

    /* Encypt the data. */
    if (mbedtls_aes_crypt_cbc(aes, mode, FS_BLOCK_SIZE, iv, in, out) != 0)
        goto done;

    rc = 0;

done:

    return rc;
}

static int _block_dev_release(fs_block_dev_t* dev)
{
    int ret = -1;
    block_dev_t* device = (block_dev_t*)dev;
    size_t new_ref_count;

    if (!device)
        goto done;

    pthread_spin_lock(&device->lock);
    new_ref_count = --device->ref_count;
    pthread_spin_unlock(&device->lock);

    if (new_ref_count == 0)
    {
        mbedtls_aes_free(&device->enc_aes);
        mbedtls_aes_free(&device->dec_aes);
        device->next->release(device->next);
        free(device);
    }

    ret = 0;

done:
    return ret;
}

static int _block_dev_get(
    fs_block_dev_t* dev, uint32_t blkno, fs_block_t* block)
{
    int ret = -1;
    block_dev_t* device = (block_dev_t*)dev;
    fs_block_t encrypted;

    if (!device || !block)
        goto done;

    /* Delegate to the next block device in the chain. */
    if (device->next->get(device->next, blkno, &encrypted) != 0)
        goto done;

    /* Decrypt the block */
    if (_crypt(
            &device->dec_aes,
            MBEDTLS_AES_DECRYPT,
            device->key,
            blkno,
            encrypted.data,
            block->data) != 0)
    {
        goto done;
    }

    ret = 0;

done:

    return ret;
}

static int _block_dev_put(
    fs_block_dev_t* dev, uint32_t blkno, const fs_block_t* block)
{
    int ret = -1;
    block_dev_t* device = (block_dev_t*)dev;
    fs_block_t encrypted;

    if (!device || !block)
        goto done;

    /* Encrypt the block */
    if (_crypt(
            &device->enc_aes,
            MBEDTLS_AES_ENCRYPT,
            device->key,
            blkno,
            block->data,
            encrypted.data) != 0)
    {
        goto done;
    }

    /* Delegate to the next block device in the chain. */
    if (device->next->put(device->next, blkno, &encrypted) != 0)
        goto done;

    ret = 0;

done:

    return ret;
}

static int _block_dev_add_ref(fs_block_dev_t* dev)
{
    int ret = -1;
    block_dev_t* device = (block_dev_t*)dev;

    if (!device)
        goto done;

    pthread_spin_lock(&device->lock);
    device->ref_count++;
    pthread_spin_unlock(&device->lock);

    ret = 0;

done:
    return ret;
}

int oe_open_crypto_block_dev(
    fs_block_dev_t** block_dev,
    const uint8_t key[FS_KEY_SIZE],
    fs_block_dev_t* next)
{
    int ret = -1;
    block_dev_t* device = NULL;

    if (block_dev)
        *block_dev = NULL;

    if (!block_dev || !next)
        goto done;

    if (!(device = calloc(1, sizeof(block_dev_t))))
        goto done;

    device->base.get = _block_dev_get;
    device->base.put = _block_dev_put;
    device->base.add_ref = _block_dev_add_ref;
    device->base.release = _block_dev_release;
    device->ref_count = 1;
    device->next = next;
    memcpy(device->key, key, FS_KEY_SIZE);
    mbedtls_aes_init(&device->enc_aes);
    mbedtls_aes_init(&device->dec_aes);

    /* Initialize an AES context for encrypting. */
    if (mbedtls_aes_setkey_enc(&device->enc_aes, key, FS_KEY_SIZE * 8) != 0)
    {
        mbedtls_aes_free(&device->enc_aes);
        goto done;
    }

    /* Initialize an AES context for decrypting. */
    if (mbedtls_aes_setkey_dec(&device->dec_aes, key, FS_KEY_SIZE * 8) != 0)
    {
        mbedtls_aes_free(&device->enc_aes);
        mbedtls_aes_free(&device->dec_aes);
        goto done;
    }

    next->add_ref(next);

    *block_dev = &device->base;
    device = NULL;

    ret = 0;

done:

    if (device)
        free(device);

    return ret;
}
