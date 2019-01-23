// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../common/hostfsargs.h"

void (*oe_handle_hostfs_ocall_callback)(void*);

static void _handle_hostfs_ocall(void* args_)
{
    oe_hostfs_args_t* args = (oe_hostfs_args_t*)args_;

    if (!args)
        return;

    switch (args->op)
    {
        case OE_HOSTFS_OP_NONE:
        {
            break;
        }
        case OE_HOSTFS_OP_OPEN:
        {
            args->u.open.ret = open(
                args->u.open.pathname, args->u.open.flags, args->u.open.mode);
            break;
        }
        case OE_HOSTFS_OP_CLOSE:
        {
            args->u.close.ret = close(args->u.close.fd);
            break;
        }
        case OE_HOSTFS_OP_READ:
        {
            args->u.read.ret =
                read(args->u.read.fd, args->buf, args->u.read.count);
            break;
        }
        case OE_HOSTFS_OP_WRITE:
        {
            args->u.read.ret =
                write(args->u.read.fd, args->buf, args->u.read.count);
            break;
        }
        case OE_HOSTFS_OP_LSEEK:
        {
            args->u.lseek.ret = lseek(
                args->u.lseek.fd, args->u.lseek.offset, args->u.lseek.whence);
            break;
        }
        case OE_HOSTFS_OP_OPENDIR:
        {
            args->u.opendir.ret = opendir(args->u.opendir.name);
            break;
        }
        case OE_HOSTFS_OP_READDIR:
        {
            struct dirent entry;
            struct dirent* result = NULL;
            args->u.readdir.ret =
                readdir_r(args->u.readdir.dirp, &entry, &result);

            if (args->u.readdir.ret == 0 && result)
            {
                args->u.readdir.entry.d_ino = result->d_ino;
                args->u.readdir.entry.d_off = result->d_off;
                args->u.readdir.entry.d_reclen = result->d_reclen;
                args->u.readdir.entry.d_type = result->d_type;

                *args->u.readdir.entry.d_name = '\0';

                strncat(
                    args->u.readdir.entry.d_name,
                    result->d_name,
                    sizeof(args->u.readdir.entry.d_name) - 1);
            }
            else
            {
                memset(
                    &args->u.readdir.entry, 0, sizeof(args->u.readdir.entry));
            }
            break;
        }
        case OE_HOSTFS_OP_CLOSEDIR:
        {
            args->u.closedir.ret = closedir(args->u.closedir.dirp);
            break;
        }
        case OE_HOSTFS_OP_STAT:
        {
            struct stat buf;

            if ((args->u.stat.ret = stat(args->u.stat.pathname, &buf)) == 0)
            {
                args->u.stat.buf.st_dev = buf.st_dev;
                args->u.stat.buf.st_ino = buf.st_ino;
                args->u.stat.buf.st_mode = buf.st_mode;
                args->u.stat.buf.st_nlink = buf.st_nlink;
                args->u.stat.buf.st_uid = buf.st_uid;
                args->u.stat.buf.st_gid = buf.st_gid;
                args->u.stat.buf.st_rdev = buf.st_rdev;
                args->u.stat.buf.st_size = buf.st_size;
                args->u.stat.buf.st_blksize = buf.st_blksize;
                args->u.stat.buf.st_blocks = buf.st_blocks;
                args->u.stat.buf.st_atim.tv_sec = buf.st_atim.tv_sec;
                args->u.stat.buf.st_atim.tv_nsec = buf.st_atim.tv_nsec;
                args->u.stat.buf.st_mtim.tv_sec = buf.st_mtim.tv_sec;
                args->u.stat.buf.st_mtim.tv_nsec = buf.st_mtim.tv_nsec;
                args->u.stat.buf.st_ctim.tv_sec = buf.st_ctim.tv_sec;
                args->u.stat.buf.st_ctim.tv_nsec = buf.st_ctim.tv_nsec;
            }
            else
            {
                memset(&args->u.stat.buf, 0, sizeof(args->u.stat.buf));
            }
            break;
        }
        case OE_HOSTFS_OP_UNLINK:
        {
            args->u.unlink.ret = unlink(args->u.unlink.pathname);
            break;
        }
        case OE_HOSTFS_OP_LINK:
        {
            args->u.link.ret = link(args->u.link.oldpath, args->u.link.newpath);
            break;
        }
        case OE_HOSTFS_OP_RENAME:
        {
            args->u.rename.ret =
                rename(args->u.rename.oldpath, args->u.rename.newpath);
            break;
        }
        case OE_HOSTFS_OP_MKDIR:
        {
            args->u.mkdir.ret =
                mkdir(args->u.mkdir.pathname, args->u.mkdir.mode);
            break;
        }
        case OE_HOSTFS_OP_RMDIR:
        {
            args->u.rmdir.ret = rmdir(args->u.rmdir.pathname);
            break;
        }
        case OE_HOSTFS_OP_TRUNCATE:
        {
            args->u.truncate.ret = rmdir(args->u.truncate.path);
            break;
        }
    }
}

void oe_install_hostfs(void)
{
    oe_handle_hostfs_ocall_callback = _handle_hostfs_ocall;
}
