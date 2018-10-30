// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#ifndef _OE_MOUNT_H
#define _OE_MOUNT_H

#include <openenclave/bits/defs.h>
#include <openenclave/bits/result.h>

OE_EXTERNC_BEGIN

#define OE_MOUNT_FLAG_NONE 0
#define OE_MOUNT_FLAG_MKFS 1
#define OE_MOUNT_FLAG_CRYPTO 2

int oe_mount_oefs(
    const char* source,
    const char* target,
    uint32_t flags,
    size_t num_blocks);

int oe_unmount(const char* target);

OE_EXTERNC_END

#endif /* _OE_MOUNT_H */
