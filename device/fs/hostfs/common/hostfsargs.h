// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#ifndef _OE_HOSTFSARGS_H
#define _OE_HOSTFSARGS_H

#include <openenclave/internal/device.h>

OE_EXTERNC_BEGIN

#define OE_HOSTFS_MODE_MAX 8
#define OE_HOSTFS_PATH_MAX 1024

typedef enum _oe_hostfs_op
{
    OE_HOSTFS_OP_NONE,
    OE_HOSTFS_OP_OPEN,
    OE_HOSTFS_OP_READ,
    OE_HOSTFS_OP_WRITE,
    OE_HOSTFS_OP_LSEEK,
    OE_HOSTFS_OP_CLOSE,
    OE_HOSTFS_OP_OPENDIR,
    OE_HOSTFS_OP_READDIR,
    OE_HOSTFS_OP_CLOSEDIR,
    OE_HOSTFS_OP_STAT,
    OE_HOSTFS_OP_LINK,
    OE_HOSTFS_OP_UNLINK,
    OE_HOSTFS_OP_RENAME,
    OE_HOSTFS_OP_TRUNCATE,
    OE_HOSTFS_OP_MKDIR,
    OE_HOSTFS_OP_RMDIR,
} oe_hostfs_op_t;

typedef struct _oe_hostfs_args
{
    oe_hostfs_op_t op;
    int err;
    union
    {
        struct
        {
            int ret;
            char pathname[OE_PATH_MAX];
            int flags;
            oe_mode_t mode;
        }
        open;
        struct
        {
            ssize_t ret;
            int fd;
            size_t count;
        }
        read;
        struct
        {
            ssize_t ret;
            int fd;
            size_t count;
        }
        write;
        struct
        {
            oe_off_t ret;
            int fd;
            oe_off_t offset;
            int whence;
        }
        lseek;
        struct
        {
            int ret;
            int fd;
        }
        close;
        struct
        {
            oe_dir_t* ret;
            char name[OE_PATH_MAX];
        }
        opendir;
        struct
        {
            int ret;
            oe_dir_t* dirp;
            struct oe_dirent entry;
        }
        readdir;
        struct
        {
            int ret;
            oe_dir_t* dirp;
        }
        closedir;
        struct
        {
            int ret;
            char pathname[OE_PATH_MAX];
            struct oe_stat buf;
        }
        stat;
        struct
        {
            int ret;
            int fd;
            struct oe_stat buf;
        }
        fstat;
        struct
        {
            int ret;
            char oldpath[OE_PATH_MAX];
            char newpath[OE_PATH_MAX];
        }
        link;
        struct
        {
            int ret;
            char pathname[OE_PATH_MAX];
        }
        unlink;
        struct
        {
            int ret;
            char oldpath[OE_PATH_MAX];
            char newpath[OE_PATH_MAX];
        }
        rename;
        struct
        {
            int ret;
            char path[OE_PATH_MAX];
            oe_off_t length;
        }
        truncate;
    } u;
    uint8_t buf[];
} oe_hostfs_args_t;

OE_EXTERNC_END

#endif /* _OE_HOSTFSARGS_H */
