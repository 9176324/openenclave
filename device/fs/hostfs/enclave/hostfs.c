// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#define _GNU_SOURCE

// clang-format off
#include <openenclave/enclave.h>
// clang-format on

#include <openenclave/internal/device.h>
#include <openenclave/internal/fs_ops.h>
#include <openenclave/bits/safemath.h>
#include <openenclave/internal/calls.h>
#include <openenclave/internal/thread.h>
#include <openenclave/internal/atexit.h>
#include <openenclave/internal/enclavelibc.h>
#include "../../common/hostbatch.h"
#include "../common/hostfsargs.h"

/*
**==============================================================================
**
** host batch:
**
**==============================================================================
*/

static oe_host_batch_t* _host_batch;
static oe_spinlock_t _lock;

static void _atexit_handler()
{
    oe_spin_lock(&_lock);
    oe_host_batch_delete(_host_batch);
    _host_batch = NULL;
    oe_spin_unlock(&_lock);
}

static oe_host_batch_t* _get_host_batch(void)
{
    const size_t BATCH_SIZE = sizeof(oe_hostfs_args_t) + OE_BUFSIZ;

    if (_host_batch == NULL)
    {
        oe_spin_lock(&_lock);

        if (_host_batch == NULL)
        {
            _host_batch = oe_host_batch_new(BATCH_SIZE);
            oe_atexit(_atexit_handler);
        }

        oe_spin_unlock(&_lock);
    }

    return _host_batch;
}

/*
**==============================================================================
**
** hostfs operations:
**
**==============================================================================
*/

#define FS_MAGIC   0x5f35f964
#define FILE_MAGIC 0xfe48c6ff
#define DIR_MAGIC  0x8add1b0b

typedef oe_hostfs_args_t args_t;

typedef struct _fs
{
    oe_device_t base;
    uint32_t magic;
}
fs_t;

typedef struct _file
{
    oe_device_t base;
    uint32_t magic;
    int host_fd;
}
file_t;

typedef struct _dir
{
    oe_device_t base;
    uint32_t magic;
    void* host_dir;
    struct oe_dirent entry;
}
dir_t;

static fs_t* _cast_fs(const oe_device_t* device)
{
    fs_t* fs = (fs_t*)device;

    if (fs == NULL || fs->magic != FS_MAGIC)
        return NULL;

    return fs;
}

static file_t* _cast_file(const oe_device_t* device)
{
    file_t* file = (file_t*)device;

    if (file == NULL || file->magic != FILE_MAGIC)
        return NULL;

    return file;
}

static dir_t* _cast_dir(const oe_device_t* device)
{
    dir_t* dir = (dir_t*)device;

    if (dir == NULL || dir->magic != DIR_MAGIC)
        return NULL;

    return dir;
}

static ssize_t _hostfs_read(oe_device_t*, void *buf, size_t count);

static int _hostfs_close(oe_device_t*);

static oe_device_t* _hostfs_open(
    oe_device_t* fs_,
    const char *pathname,
    int flags,
    oe_mode_t mode)
{
    oe_device_t* ret = NULL;
    fs_t* fs = _cast_fs(fs_);
    args_t* args = NULL;
    file_t* file = NULL;
    oe_host_batch_t* batch = _get_host_batch();

    oe_errno = 0;

    /* Check parameters */
    if (!fs || !pathname || !batch)
    {
        oe_errno = OE_EINVAL;
        goto done;
    }

    /* Input */
    {
        if (!(args = oe_host_batch_calloc(batch, sizeof(args_t))))
        {
            oe_errno = OE_ENOMEM;
            goto done;
        }

        args->op = OE_HOSTFS_OP_OPEN;
        args->u.open.ret = -1;

        if (oe_strlcpy(
            args->u.open.pathname, pathname, OE_PATH_MAX) >= OE_PATH_MAX)
        {
            oe_errno = OE_ENAMETOOLONG;
            goto done;
        }

        args->u.open.flags = flags;
        args->u.open.mode = mode;
    }

    /* Call */
    {
        if (oe_ocall(OE_OCALL_HOSTFS, (uint64_t)args, NULL) != OE_OK)
        {
            oe_errno = OE_EINVAL;
            goto done;
        }

        if (args->u.open.ret < 0)
        {
            oe_errno = args->err;
            goto done;
        }
    }

    /* Output */
    {
        if (!(file = oe_calloc(1, sizeof(file_t))))
        {
            oe_errno = OE_ENOMEM;
            goto done;
        }

        file->base.type = OE_DEV_HOST_FILE;
        file->base.size = sizeof(file_t);
        file->magic = FILE_MAGIC;
        file->base.ops.fs = fs->base.ops.fs;
        file->host_fd = args->u.open.ret;
    }

    ret = &file->base;
    file = NULL;

done:

    if (file)
        oe_free(file);

    if (args)
        oe_host_batch_free(batch);

    return ret;
}

static ssize_t _hostfs_read(oe_device_t* file_, void *buf, size_t count)
{
    int ret = -1;
    file_t* file = _cast_file(file_);
    oe_host_batch_t* batch = _get_host_batch();
    args_t* args = NULL;

    oe_errno = 0;

    /* Check parameters. */
    if (!file || !batch || (count && !buf))
    {
        oe_errno = OE_EINVAL;
        goto done;
    }

    /* Input */
    {
        if (!(args = oe_host_batch_calloc(batch, sizeof(args_t) + count)))
        {
            oe_errno = OE_ENOMEM;
            goto done;
        }

        args->op = OE_HOSTFS_OP_READ;
        args->u.read.ret = -1;
        args->u.read.count = count;
    }

    /* Call */
    {
        if (oe_ocall(OE_OCALL_HOSTFS, (uint64_t)args, NULL) != OE_OK)
        {
            oe_errno = OE_EINVAL;
            goto done;
        }

        if ((ret = args->u.open.ret) == -1)
        {
            oe_errno = args->err;
            goto done;
        }
    }

    /* Output */
    {
        oe_memcpy(buf, args->buf, count);
    }

done:
    return ret;
}

static ssize_t _hostfs_write(
    oe_device_t* file_,
    const void *buf,
    size_t count)
{
    int ret = -1;
    file_t* file = _cast_file(file_);
    oe_host_batch_t* batch = _get_host_batch();
    args_t* args = NULL;

    oe_errno = 0;

    /* Check parameters. */
    if (!file || !batch || (count && !buf))
    {
        oe_errno = OE_EINVAL;
        goto done;
    }

    /* Input */
    {
        if (!(args = oe_host_batch_calloc(batch, sizeof(args_t) + count)))
        {
            oe_errno = OE_ENOMEM;
            goto done;
        }

        args->op = OE_HOSTFS_OP_WRITE;
        args->u.write.ret = -1;
        args->u.write.fd = file->host_fd;
        args->u.write.count = count;
        oe_memcpy(args->buf, buf, count);
    }

    /* Call */
    {
        if (oe_ocall(OE_OCALL_HOSTFS, (uint64_t)args, NULL) != OE_OK)
        {
            oe_errno = OE_EINVAL;
            goto done;
        }

        if ((ret = args->u.open.ret) == -1)
        {
            oe_errno = args->err;
            goto done;
        }
    }

    ret = 0;

done:
    return ret;
}

static oe_off_t _hostfs_lseek(oe_device_t* file_, oe_off_t offset, int whence)
{
    oe_off_t ret = -1;
    file_t* file = _cast_file(file_);
    oe_host_batch_t* batch = _get_host_batch();
    args_t* args = NULL;

    oe_errno = 0;

    /* Check parameters. */
    if (!file || !batch)
    {
        oe_errno = OE_EINVAL;
        goto done;
    }

    /* Input */
    {
        if (!(args = oe_host_batch_calloc(batch, sizeof(args_t))))
        {
            oe_errno = OE_ENOMEM;
            goto done;
        }

        args->op = OE_HOSTFS_OP_LSEEK;
        args->u.lseek.ret = -1;
        args->u.lseek.fd = file->host_fd;
        args->u.lseek.offset = offset;
        args->u.lseek.whence = whence;
    }

    /* Call */
    {
        if (oe_ocall(OE_OCALL_HOSTFS, (uint64_t)args, NULL) != OE_OK)
        {
            oe_errno = OE_EINVAL;
            goto done;
        }

        if ((ret = args->u.lseek.ret) == -1)
        {
            oe_errno = args->err;
            goto done;
        }
    }

done:
    return ret;
}

static int _hostfs_close(oe_device_t* file_)
{
    int ret = -1;
    file_t* file = _cast_file(file_);
    oe_host_batch_t* batch = _get_host_batch();
    args_t* args = NULL;

    oe_errno = 0;

    /* Check parameters. */
    if (!file || !batch)
    {
        oe_errno = OE_EINVAL;
        goto done;
    }

    /* Input */
    {
        if (!(args = oe_host_batch_calloc(batch, sizeof(args_t))))
        {
            oe_errno = OE_ENOMEM;
            goto done;
        }

        args->op = OE_HOSTFS_OP_CLOSE;
        args->u.close.ret = -1;
        args->u.close.fd = file->host_fd;
    }

    /* Call */
    {
        if (oe_ocall(OE_OCALL_HOSTFS, (uint64_t)args, NULL) != OE_OK)
        {
            oe_errno = OE_EINVAL;
            goto done;
        }

        if (args->u.close.ret != 0)
        {
            oe_errno = args->err;
            goto done;
        }
    }

    /* Release the file object. */
    oe_free(file);

    ret = 0;

done:
    return ret;
}

static int _hostfs_ioctl(oe_device_t* file, unsigned long request, ...)
{
    /* Unsupported */
    oe_errno = OE_ENOTTY;
    (void)file;
    (void)request;
    return -1;
}

static oe_device_t* _hostfs_opendir(oe_device_t* fs_, const char* name)
{
    oe_device_t* ret = NULL;
    fs_t* fs = _cast_fs(fs_);
    oe_host_batch_t* batch = _get_host_batch();
    args_t* args = NULL;
    dir_t* dir = NULL;

    /* Check parameters */
    if (!fs || !name || !batch)
    {
        oe_errno = OE_EINVAL;
        goto done;
    }

    /* Input */
    {
        if (!(args = oe_host_batch_calloc(batch, sizeof(args_t))))
        {
            oe_errno = OE_ENOMEM;
            goto done;
        }

        args->op = OE_HOSTFS_OP_OPENDIR;
        args->u.opendir.ret = NULL;
        oe_strlcpy(args->u.opendir.name, name, OE_PATH_MAX);
    }

    /* Call */
    {
        if (oe_ocall(OE_OCALL_HOSTFS, (uint64_t)args, NULL) != OE_OK)
        {
            oe_errno = OE_EINVAL;
            goto done;
        }

        if (args->u.opendir.ret == NULL)
        {
            oe_errno = args->err;
            goto done;
        }
    }

    /* Output */
    {
        if (!(dir = oe_calloc(1, sizeof(dir_t))))
        {
            oe_errno = OE_ENOMEM;
            goto done;
        }

        dir->base.type = OE_DEV_HOST_FILE;
        dir->base.size = sizeof(dir_t);
        dir->magic = DIR_MAGIC;
        dir->base.ops.fs = fs->base.ops.fs;
        dir->host_dir = args->u.opendir.ret;
    }

    ret = &dir->base;
    dir = NULL;

done:

    if (args)
        oe_host_batch_free(batch);

    if (dir)
        oe_free(dir);

    return ret;
}

static struct oe_dirent* _hostfs_readdir(oe_device_t* dir_)
{
    struct oe_dirent* ret = NULL;
    dir_t* dir = _cast_dir(dir_);
    oe_host_batch_t* batch = _get_host_batch();
    args_t* args = NULL;

    /* Check parameters */
    if (!dir || !batch)
    {
        oe_errno = OE_EINVAL;
        goto done;
    }

    /* Input */
    {
        if (!(args = oe_host_batch_calloc(batch, sizeof(args_t))))
            goto done;

        args->u.readdir.ret = -1;
        args->op = OE_HOSTFS_OP_READDIR;
        args->u.readdir.dirp = dir->host_dir;
    }

    /* Call */
    {
        if (oe_ocall(OE_OCALL_HOSTFS, (uint64_t)args, NULL) != OE_OK)
            goto done;

        if (args->u.readdir.ret != 0)
        {
            oe_errno = args->err;
            goto done;
        }
    }

    /* Output */
    {
        dir->entry = args->u.readdir.entry;
        ret = &dir->entry;
    }

done:

    if (args)
        oe_host_batch_free(batch);

    return ret;
}

static int _hostfs_closedir(oe_device_t* dir_)
{
    int ret = -1;
    dir_t* dir = _cast_dir(dir_);
    oe_host_batch_t* batch = _get_host_batch();
    args_t* args = NULL;

    /* Check parameters */
    if (!dir || !batch)
    {
        oe_errno = OE_EINVAL;
        goto done;
    }

    /* Input */
    {
        if (!(args = oe_host_batch_calloc(batch, sizeof(args_t))))
            goto done;

        args->op = OE_HOSTFS_OP_CLOSEDIR;
        args->u.closedir.ret = -1;
        args->u.closedir.dirp = dir->host_dir;
    }

    /* Call */
    {
        if (oe_ocall(OE_OCALL_HOSTFS, (uint64_t)args, NULL) != OE_OK)
            goto done;

        if ((ret = args->u.closedir.ret) != 0)
        {
            oe_errno = args->err;
            goto done;
        }
    }

    oe_free(dir);

done:

    if (args)
        oe_host_batch_free(batch);

    return ret;
}

static int _hostfs_stat(
    oe_device_t* fs_,
    const char* pathname,
    struct oe_stat* buf)
{
    int ret = -1;
    fs_t* fs = _cast_fs(fs_);
    oe_host_batch_t* batch = _get_host_batch();
    args_t* args = NULL;

    /* Check parameters */
    if (!fs || !pathname || !buf || !batch)
    {
        oe_errno = OE_EINVAL;
        goto done;
    }

    /* Input */
    {
        if (!(args = oe_host_batch_calloc(batch, sizeof(args_t))))
            goto done;

        args->op = OE_HOSTFS_OP_STAT;
        args->u.stat.ret = -1;

        if (oe_strlcpy(
            args->u.stat.pathname, pathname, OE_PATH_MAX) >= OE_PATH_MAX)
        {
            oe_errno = OE_ENAMETOOLONG;
            goto done;
        }
    }

    /* Call */
    {
        if (oe_ocall(OE_OCALL_HOSTFS, (uint64_t)args, NULL) != OE_OK)
            goto done;

        if ((ret = args->u.stat.ret) != 0)
        {
            oe_errno = args->err;
            goto done;
        }
    }

    /* Output */
    {
        *buf = args->u.stat.buf;
    }

done:

    if (args)
        oe_host_batch_free(batch);

    return ret;
}

static int _hostfs_link(
    oe_device_t* fs_,
    const char* oldpath,
    const char* newpath)
{
    int ret = -1;
    fs_t* fs = _cast_fs(fs_);
    oe_host_batch_t* batch = _get_host_batch();
    args_t* args = NULL;

    /* Check parameters */
    if (!fs || !oldpath || !newpath || !batch)
    {
        oe_errno = OE_EINVAL;
        goto done;
    }

    /* Input */
    {
        if (!(args = oe_host_batch_calloc(batch, sizeof(args_t))))
            goto done;

        args->op = OE_HOSTFS_OP_LINK;
        args->u.link.ret = -1;

        if (oe_strlcpy(
            args->u.link.oldpath, oldpath, OE_PATH_MAX) >= OE_PATH_MAX)
        {
            oe_errno = OE_ENAMETOOLONG;
            goto done;
        }

        if (oe_strlcpy(
            args->u.link.newpath, newpath, OE_PATH_MAX) >= OE_PATH_MAX)
        {
            oe_errno = OE_ENAMETOOLONG;
            goto done;
        }
    }

    /* Call */
    {
        if (oe_ocall(OE_OCALL_HOSTFS, (uint64_t)args, NULL) != OE_OK)
            goto done;

        if ((ret = args->u.link.ret) != 0)
        {
            oe_errno = args->err;
            goto done;
        }
    }

done:

    if (args)
        oe_host_batch_free(batch);

    return ret;
}

static int _hostfs_unlink(oe_device_t* fs_, const char* pathname)
{
    int ret = -1;
    fs_t* fs = _cast_fs(fs_);
    oe_host_batch_t* batch = _get_host_batch();
    args_t* args = NULL;

    /* Check parameters */
    if (!fs || !pathname || !batch)
    {
        oe_errno = OE_EINVAL;
        goto done;
    }

    /* Input */
    {
        if (!(args = oe_host_batch_calloc(batch, sizeof(args_t))))
            goto done;

        args->op = OE_HOSTFS_OP_UNLINK;
        args->u.unlink.ret = -1;

        if (oe_strlcpy(
            args->u.unlink.pathname, pathname, OE_PATH_MAX) >= OE_PATH_MAX)
        {
            oe_errno = OE_ENAMETOOLONG;
            goto done;
        }
    }

    /* Call */
    {
        if (oe_ocall(OE_OCALL_HOSTFS, (uint64_t)args, NULL) != OE_OK)
            goto done;

        if ((ret = args->u.unlink.ret) != 0)
        {
            oe_errno = args->err;
            goto done;
        }
    }

done:

    if (args)
        oe_host_batch_free(batch);

    return ret;
}

static int _hostfs_rename(
    oe_device_t* fs_,
    const char* oldpath,
    const char* newpath)
{
    int ret = -1;
    fs_t* fs = _cast_fs(fs_);
    oe_host_batch_t* batch = _get_host_batch();
    args_t* args = NULL;

    /* Check parameters */
    if (!fs || !oldpath || !newpath || !batch)
    {
        oe_errno = OE_EINVAL;
        goto done;
    }

    /* Input */
    {
        if (!(args = oe_host_batch_calloc(batch, sizeof(args_t))))
            goto done;

        args->op = OE_HOSTFS_OP_RENAME;
        args->u.rename.ret = -1;

        if (oe_strlcpy(
            args->u.rename.oldpath, oldpath, OE_PATH_MAX) >= OE_PATH_MAX)
        {
            oe_errno = OE_ENAMETOOLONG;
            goto done;
        }

        if (oe_strlcpy(
            args->u.rename.newpath, newpath, OE_PATH_MAX) >= OE_PATH_MAX)
        {
            oe_errno = OE_ENAMETOOLONG;
            goto done;
        }
    }

    /* Call */
    {
        if (oe_ocall(OE_OCALL_HOSTFS, (uint64_t)args, NULL) != OE_OK)
            goto done;

        if ((ret = args->u.rename.ret) != 0)
        {
            oe_errno = args->err;
            goto done;
        }
    }

done:

    if (args)
        oe_host_batch_free(batch);

    return ret;
}

static int _hostfs_truncate(oe_device_t* fs_, const char* path, oe_off_t length)
{
    int ret = -1;
    fs_t* fs = _cast_fs(fs_);
    oe_host_batch_t* batch = _get_host_batch();
    args_t* args = NULL;

    /* Check parameters */
    if (!fs || !path || !batch)
    {
        oe_errno = OE_EINVAL;
        goto done;
    }

    /* Input */
    {
        if (!(args = oe_host_batch_calloc(batch, sizeof(args_t))))
            goto done;

        args->op = OE_HOSTFS_OP_TRUNCATE;
        args->u.truncate.ret = -1;
        args->u.truncate.length = length;

        if (oe_strlcpy(
            args->u.truncate.path, path, OE_PATH_MAX) >= OE_PATH_MAX)
        {
            oe_errno = OE_ENAMETOOLONG;
            goto done;
        }
    }

    /* Call */
    {
        if (oe_ocall(OE_OCALL_HOSTFS, (uint64_t)args, NULL) != OE_OK)
            goto done;

        if ((ret = args->u.truncate.ret) != 0)
        {
            oe_errno = args->err;
            goto done;
        }
    }

done:

    if (args)
        oe_host_batch_free(batch);

    return ret;
}

static int _hostfs_mkdir(oe_device_t* fs_, const char* pathname, oe_mode_t mode)
{
    int ret = -1;
    fs_t* fs = _cast_fs(fs_);
    oe_host_batch_t* batch = _get_host_batch();
    args_t* args = NULL;

    /* Check parameters */
    if (!fs || !pathname || !batch)
    {
        oe_errno = OE_EINVAL;
        goto done;
    }

    /* Input */
    {
        if (!(args = oe_host_batch_calloc(batch, sizeof(args_t))))
            goto done;

        args->op = OE_HOSTFS_OP_MKDIR;
        args->u.mkdir.ret = -1;
        args->u.mkdir.mode = mode;

        if (oe_strlcpy(
            args->u.mkdir.pathname, pathname, OE_PATH_MAX) >= OE_PATH_MAX)
        {
            oe_errno = OE_ENAMETOOLONG;
            goto done;
        }
    }

    /* Call */
    {
        if (oe_ocall(OE_OCALL_HOSTFS, (uint64_t)args, NULL) != OE_OK)
            goto done;

        if ((ret = args->u.mkdir.ret) != 0)
        {
            oe_errno = args->err;
            goto done;
        }
    }

done:

    if (args)
        oe_host_batch_free(batch);

    return ret;
}

static int _hostfs_rmdir(oe_device_t* fs_, const char* pathname)
{
    int ret = -1;
    fs_t* fs = _cast_fs(fs_);
    oe_host_batch_t* batch = _get_host_batch();
    args_t* args = NULL;

    /* Check parameters */
    if (!fs || !pathname || !batch)
    {
        oe_errno = OE_EINVAL;
        goto done;
    }

    /* Input */
    {
        if (!(args = oe_host_batch_calloc(batch, sizeof(args_t))))
            goto done;

        args->op = OE_HOSTFS_OP_RMDIR;
        args->u.rmdir.ret = -1;

        if (oe_strlcpy(
            args->u.rmdir.pathname, pathname, OE_PATH_MAX) >= OE_PATH_MAX)
        {
            oe_errno = OE_ENAMETOOLONG;
            goto done;
        }
    }

    /* Call */
    {
        if (oe_ocall(OE_OCALL_HOSTFS, (uint64_t)args, NULL) != OE_OK)
            goto done;

        if ((ret = args->u.rmdir.ret) != 0)
        {
            oe_errno = args->err;
            goto done;
        }
    }

done:

    if (args)
        oe_host_batch_free(batch);

    return ret;
}

oe_device_t* new_hostfs(void)
{
    fs_t* ret = NULL;

    if ((ret = oe_calloc(1, sizeof(fs_t))))
        goto done;

    ret->base.type = OE_DEV_HOST_FILE;
    ret->base.size = sizeof(fs_t);
    ret->base.ops.fs.base.ioctl = _hostfs_ioctl;
    ret->base.ops.fs.open = _hostfs_open;
    ret->base.ops.fs.base.read = _hostfs_read;
    ret->base.ops.fs.base.write = _hostfs_write;
    ret->base.ops.fs.lseek = _hostfs_lseek;
    ret->base.ops.fs.base.close = _hostfs_close;
    ret->base.ops.fs.opendir = _hostfs_opendir;
    ret->base.ops.fs.readdir = _hostfs_readdir;
    ret->base.ops.fs.closedir = _hostfs_closedir;
    ret->base.ops.fs.stat = _hostfs_stat;
    ret->base.ops.fs.link = _hostfs_link;
    ret->base.ops.fs.unlink = _hostfs_unlink;
    ret->base.ops.fs.rename = _hostfs_rename;
    ret->base.ops.fs.truncate = _hostfs_truncate;
    ret->base.ops.fs.mkdir = _hostfs_mkdir;
    ret->base.ops.fs.rmdir = _hostfs_rmdir;
    ret->magic = FS_MAGIC;

    /* ATTN: OE_DEV_HOST_FILE not needed: should be just OE_DEV_FILE. */

done:
    return &ret->base;
}
