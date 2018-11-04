// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#define _GNU_SOURCE
#include "fs.h"
#include <dirent.h>
#include <pthread.h>
#include <stdbool.h>
#include <string.h>
#include "oefs.h"
#include "raise.h"
#include "syscall.h"

#define MAX_FILES 1024
#define MAX_MOUNTS 64

/* Offset to account for stdin=0, stdout=1, stderr=2. */
#define FD_OFFSET 3

#define DIR_HANDLE_MAGIC 0x173df89e

typedef struct _binding
{
    fs_t* fs;
    char path[FS_PATH_MAX];
} binding_t;

typedef struct _handle
{
    fs_t* fs;
    fs_file_t* file;
} handle_t;

typedef struct _dir
{
    uint32_t magic;
    fs_t* fs;
    fs_dir_t* dir;
    struct dirent entry;
} dir_handle_t;

static binding_t _bindings[MAX_MOUNTS];
static size_t _num_bindings;
static pthread_spinlock_t _lock;

static char _cwd[FS_PATH_MAX] = "/";
static pthread_spinlock_t lock;

static handle_t _handles[MAX_FILES];

/* Check that the path is normalized (see notes in function). */
static bool _is_path_normalized(const char* path)
{
    bool ret = false;
    char buf[FS_PATH_MAX];
    char* p;
    char* save;

    if (!path || strlen(path) >= FS_PATH_MAX)
        goto done;

    strlcpy(buf, path, FS_PATH_MAX);

    /* The path must begin with a slash. */
    if (buf[0] != '/')
        goto done;

    /* If this is the root directory. */
    if (buf[1] == '\0')
    {
        ret = true;
        goto done;
    }

    for (const char* p = buf; *p; p++)
    {
        /* The last character must not be a slash. */
        if (p[0] == '/' && p[1] == '\0')
            goto done;

        /* The path may not have consecutive slashes. */
        if (p[0] == '/' && p[1] == '/')
            goto done;
    }

    /* The path may not have "." and ".." components. */
    for (p = strtok_r(buf, "/", &save); p; p = strtok_r(NULL, "/", &save))
    {
        if (strcmp(p, ".") == 0 || strcmp(p, "..") == 0)
            goto done;
    }

    ret = true;

done:
    return ret;
}

int fs_bind(fs_t* fs, const char* path)
{
    int ret = -1;
    binding_t binding;

    if (!fs || !path || !_is_path_normalized(path))
        goto done;

    pthread_spin_lock(&_lock);
    {
        if (_num_bindings == MAX_MOUNTS)
            goto done;

        /* Check whether path is already in use. */
        for (size_t i = 0; i < _num_bindings; i++)
        {
            if (strcmp(_bindings[i].path, path) == 0)
            {
                pthread_spin_unlock(&_lock);
                goto done;
            }
        }

        /* Add the new binding. */
        strlcpy(binding.path, path, FS_PATH_MAX);
        binding.fs = fs;
        _bindings[_num_bindings++] = binding;
    }
    pthread_spin_unlock(&_lock);

    ret = 0;

done:

    return ret;
}

int fs_unbind(const char* path)
{
    int ret = -1;
    fs_t* fs = NULL;

    if (!path)
        goto done;

    pthread_spin_lock(&_lock);
    {
        /* Find the binding for this path. */
        for (size_t i = 0; i < _num_bindings; i++)
        {
            if (strcmp(_bindings[i].path, path) == 0)
            {
                fs = _bindings[i].fs;
                _bindings[i] = _bindings[_num_bindings - 1];
                _num_bindings--;
                break;
            }
        }
    }
    pthread_spin_unlock(&_lock);

    if (!fs)
        goto done;

    fs->fs_release(fs);

    ret = 0;

done:

    return ret;
}

fs_t* fs_lookup(const char* path, char suffix[FS_PATH_MAX])
{
    fs_t* ret = NULL;
    size_t match_len = 0;

    if (!path)
        goto done;

    pthread_spin_lock(&_lock);
    {
        /* Find the longest binding point that contains this path. */
        for (size_t i = 0; i < _num_bindings; i++)
        {
            size_t len = strlen(_bindings[i].path);

            if (strncmp(_bindings[i].path, path, len) == 0 &&
                (path[len] == '/' || path[len] == '\0'))
            {
                if (len > match_len)
                {
                    if (suffix)
                        strlcpy(suffix, path + len, FS_PATH_MAX);

                    match_len = len;
                    ret = _bindings[i].fs;
                }
            }
        }
    }
    pthread_spin_unlock(&_lock);

done:

    return ret;
}

static size_t _assign_handle()
{
    for (size_t i = 0; i < MAX_FILES; i++)
    {
        if (_handles[i].fs == NULL && _handles[i].file == NULL)
            return i;
    }

    return (size_t)-1;
}

static handle_t* _fd_to_handle(int fd)
{
    handle_t* h;
    size_t index = fd - FD_OFFSET;

    if (index < 0 || index >= MAX_FILES)
        return NULL;

    h = &_handles[index];

    if (!h->fs || !h->file)
        return NULL;

    return h;
}

static fs_errno_t _realpath(const char* path, char real_path[FS_PATH_MAX])
{
    fs_errno_t err = 0;
    char buf[FS_PATH_MAX];
    const char* in[FS_PATH_MAX];
    size_t nin = 0;
    const char* out[FS_PATH_MAX];
    size_t nout = 0;
    char resolved[FS_PATH_MAX];

    if (!path || !real_path)
        RAISE(FS_EINVAL);

    if (path[0] == '/')
    {
        if (strlcpy(buf, path, sizeof(buf)) >= sizeof(buf))
            RAISE(FS_ENAMETOOLONG);
    }
    else
    {
        char cwd[FS_PATH_MAX];
        int r;

        if (fs_getcwd(cwd, sizeof(cwd), &r) != 0 || r != 0)
            RAISE(FS_ENAMETOOLONG);

        if (strlcpy(buf, cwd, sizeof(buf)) >= sizeof(buf))
            RAISE(FS_ENAMETOOLONG);

        if (strlcat(buf, "/", sizeof(buf)) >= sizeof(buf))
            RAISE(FS_ENAMETOOLONG);

        if (strlcat(buf, path, sizeof(buf)) >= sizeof(buf))
            RAISE(FS_ENAMETOOLONG);
    }

    /* Split the path into elements. */
    {
        char* p;
        char* save;

        in[nin++] = "/";

        for (p = strtok_r(buf, "/", &save); p; p = strtok_r(NULL, "/", &save))
            in[nin++] = p;
    }

    /* Normalize the path. */
    for (size_t i = 0; i < nin; i++)
    {
        /* Skip "." elements. */
        if (strcmp(in[i], ".") == 0)
            continue;

        /* If "..", remove previous element. */
        if (strcmp(in[i], "..") == 0)
        {
            if (nout)
                nout--;
            continue;
        }

        out[nout++] = in[i];
    }

    /* Build the resolved path. */
    {
        *resolved = '\0';

        for (size_t i = 0; i < nout; i++)
        {
            if (strlcat(resolved, out[i], FS_PATH_MAX) >= FS_PATH_MAX)
                RAISE(FS_ENAMETOOLONG);

            if (i != 0 && i + 1 != nout)
            {
                if (strlcat(resolved, "/", FS_PATH_MAX) >= FS_PATH_MAX)
                    RAISE(FS_ENAMETOOLONG);
            }
        }
    }

    if (strlcpy(real_path, resolved, FS_PATH_MAX) >= FS_PATH_MAX)
        RAISE(FS_ENAMETOOLONG);

done:
    return err;
}

fs_errno_t fs_open(const char* pathname, int flags, uint32_t mode, int* ret)
{
    fs_errno_t err = 0;
    fs_t* fs = NULL;
    char suffix[FS_PATH_MAX];
    fs_file_t* file;
    size_t index;
    char real_path[FS_PATH_MAX];

    if (ret)
        *ret = -1;

    if (!pathname || !ret)
        RAISE(FS_EINVAL);

    CHECK(_realpath(pathname, real_path));

    if (!(fs = fs_lookup(real_path, suffix)))
        RAISE(FS_ENOENT);

    if ((index = _assign_handle()) == (size_t)-1)
        RAISE(FS_EMFILE);

    CHECK(fs->fs_open(fs, suffix, flags, mode, &file));

    _handles[index].fs = fs;
    _handles[index].file = file;

    *ret = index + FD_OFFSET;

done:

    return err;
}

fs_errno_t fs_creat(const char* pathname, uint32_t mode, int* ret)
{
    int flags = FS_O_CREAT | FS_O_WRONLY | FS_O_TRUNC;
    return fs_open(pathname, flags, mode, ret);
}

fs_errno_t fs_close(int fd, int* ret)
{
    fs_errno_t err = 0;
    handle_t* h;

    if (ret)
        *ret = -1;

    if (!ret)
        RAISE(FS_EINVAL);

    if (!(h = _fd_to_handle(fd)))
        RAISE(FS_EBADF);

    CHECK(h->fs->fs_close(h->file));

    memset(h, 0, sizeof(handle_t));

    *ret = 0;

done:
    return err;
}

fs_errno_t fs_readv(int fd, const fs_iovec_t* iov, int iovcnt, ssize_t* ret)
{
    fs_errno_t err = 0;
    handle_t* h;
    ssize_t nread = 0;

    if (ret)
        *ret = -1;

    if (!iov || !ret)
        RAISE(FS_EINVAL);

    if (!(h = _fd_to_handle(fd)))
        RAISE(FS_EBADF);

    for (int i = 0; i < iovcnt; i++)
    {
        const fs_iovec_t* p = &iov[i];
        ssize_t n;

        CHECK(h->fs->fs_read(h->file, p->iov_base, p->iov_len, &n));
        nread += n;

        if (n < p->iov_len)
            break;
    }

    *ret = nread;

done:
    return err;
}

fs_errno_t fs_writev(int fd, const fs_iovec_t* iov, int iovcnt, ssize_t* ret)
{
    fs_errno_t err = 0;
    handle_t* h;
    ssize_t nwritten = 0;

    if (ret)
        *ret = -1;

    if (!iov || !ret)
        RAISE(FS_EINVAL);

    if (!(h = _fd_to_handle(fd)))
        RAISE(FS_EBADF);

    for (int i = 0; i < iovcnt; i++)
    {
        const fs_iovec_t* p = &iov[i];
        ssize_t n;

        CHECK(h->fs->fs_write(h->file, p->iov_base, p->iov_len, &n));

        if (n != p->iov_len)
            RAISE(FS_EIO);

        nwritten += n;
    }

    *ret = nwritten;

done:
    return err;
}

fs_errno_t fs_stat(const char* pathname, fs_stat_t* buf, int* ret)
{
    fs_errno_t err = 0;
    fs_t* fs;
    char suffix[FS_PATH_MAX];
    fs_stat_t stat;
    char real_path[FS_PATH_MAX];

    if (buf)
        memset(buf, 0, sizeof(fs_stat_t));

    if (ret)
        *ret = -1;

    memset(&stat, 0, sizeof(stat));

    if (!pathname || !buf || !ret)
        RAISE(FS_EINVAL);

    CHECK(_realpath(pathname, real_path));

    if (!(fs = fs_lookup(real_path, suffix)))
        RAISE(FS_ENOENT);

    CHECK(fs->fs_stat(fs, suffix, &stat));

    *buf = stat;
    *ret = 0;

done:
    return err;
}

fs_errno_t fs_lseek(int fd, ssize_t off, int whence, ssize_t* ret)
{
    fs_errno_t err = 0;
    handle_t* h;

    if (ret)
        *ret = -1;

    if (!ret)
        RAISE(FS_EINVAL);

    if (!(h = _fd_to_handle(fd)))
        RAISE(FS_EINVAL);

    CHECK(h->fs->fs_lseek(h->file, off, whence, ret));

done:
    return err;
}

fs_errno_t fs_link(const char* oldpath, const char* newpath, int* ret)
{
    fs_errno_t err = 0;
    fs_t* old_fs;
    fs_t* new_fs;
    char old_suffix[FS_PATH_MAX];
    char new_suffix[FS_PATH_MAX];
    char old_real_path[FS_PATH_MAX];
    char new_real_path[FS_PATH_MAX];

    if (ret)
        *ret = -1;

    if (!oldpath || !newpath || !ret)
        RAISE(FS_EINVAL);

    CHECK(_realpath(oldpath, old_real_path));
    CHECK(_realpath(newpath, new_real_path));

    if (!(old_fs = fs_lookup(old_real_path, old_suffix)))
        RAISE(FS_ENOENT);

    if (!(new_fs = fs_lookup(new_real_path, new_suffix)))
        RAISE(FS_ENOENT);

    /* Disallow linking across different file systems. */
    if (old_fs != new_fs)
        RAISE(FS_ENOENT);

    CHECK(old_fs->fs_link(old_fs, old_suffix, new_suffix));

    *ret = 0;

done:
    return err;
}

fs_errno_t fs_unlink(const char* pathname, int* ret)
{
    fs_errno_t err = 0;
    fs_t* fs;
    char suffix[FS_PATH_MAX];
    char real_path[FS_PATH_MAX];

    if (ret)
        *ret = -1;

    if (!pathname || !ret)
        RAISE(FS_EINVAL);

    CHECK(_realpath(pathname, real_path));

    if (!(fs = fs_lookup(real_path, suffix)))
        RAISE(FS_ENOENT);

    CHECK(fs->fs_unlink(fs, suffix));

    *ret = 0;

done:
    return err;
}

fs_errno_t fs_rename(const char* oldpath, const char* newpath, int* ret)
{
    fs_errno_t err = 0;
    fs_t* old_fs;
    fs_t* new_fs;
    char old_suffix[FS_PATH_MAX];
    char new_suffix[FS_PATH_MAX];
    char old_real_path[FS_PATH_MAX];
    char new_real_path[FS_PATH_MAX];

    if (ret)
        *ret = -1;

    if (!oldpath || !newpath || !ret)
        RAISE(FS_EINVAL);

    CHECK(_realpath(oldpath, old_real_path));
    CHECK(_realpath(newpath, new_real_path));

    if (!(old_fs = fs_lookup(old_real_path, old_suffix)))
        RAISE(FS_ENOENT);

    if (!(new_fs = fs_lookup(new_real_path, new_suffix)))
        RAISE(FS_ENOENT);

    /* Disallow renaming across different file systems. */
    if (old_fs != new_fs)
        RAISE(FS_ENOENT);

    CHECK(old_fs->fs_rename(old_fs, old_suffix, new_suffix));

    *ret = 0;

done:
    return err;
}

fs_errno_t fs_truncate(const char* path, ssize_t length, int* ret)
{
    fs_errno_t err = 0;
    fs_t* fs;
    char suffix[FS_PATH_MAX];
    char real_path[FS_PATH_MAX];

    if (ret)
        *ret = -1;

    if (!path || !ret)
        RAISE(FS_EINVAL);

    CHECK(_realpath(path, real_path));

    if (!(fs = fs_lookup(real_path, suffix)))
        RAISE(FS_ENOENT);

    CHECK(fs->fs_truncate(fs, suffix, length));

    *ret = 0;

done:
    return err;
}

fs_errno_t fs_mkdir(const char* pathname, uint32_t mode, int* ret)
{
    fs_errno_t err = 0;
    fs_t* fs;
    char suffix[FS_PATH_MAX];
    char real_path[FS_PATH_MAX];

    if (ret)
        *ret = -1;

    if (!pathname || !ret)
        RAISE(FS_EINVAL);

    CHECK(_realpath(pathname, real_path));

    if (!(fs = fs_lookup(real_path, suffix)))
        RAISE(FS_ENOENT);

    CHECK(fs->fs_mkdir(fs, suffix, mode));

    *ret = 0;

done:
    return err;
}

fs_errno_t fs_rmdir(const char* pathname, int* ret)
{
    fs_errno_t err = 0;
    fs_t* fs;
    char suffix[FS_PATH_MAX];
    char real_path[FS_PATH_MAX];

    if (ret)
        *ret = -1;

    if (!pathname || !ret)
        RAISE(FS_EINVAL);

    CHECK(_realpath(pathname, real_path));

    if (!(fs = fs_lookup(real_path, suffix)))
        RAISE(FS_ENOENT);

    CHECK(fs->fs_rmdir(fs, suffix));

    *ret = 0;

done:
    return err;
}

fs_errno_t fs_getdents(
    unsigned int fd,
    struct dirent* dirp,
    unsigned int count,
    int* ret)
{
    fs_errno_t err = 0;
    handle_t* h;
    unsigned int off = 0;
    unsigned int remaining = count;

    if (ret)
        *ret = -1;

    if (!dirp || !ret)
        RAISE(FS_EINVAL);

    if (!(h = _fd_to_handle(fd)))
        RAISE(FS_EBADF);

    while (remaining >= sizeof(struct dirent))
    {
        fs_dirent_t ent;
        ssize_t nread;

        CHECK(h->fs->fs_read(h->file, &ent, sizeof(ent), &nread));

        /* Handle end of file. */
        if (nread == 0)
            break;

        /* The file size should be a multiple of the entry size. */
        if (nread != sizeof(ent))
            RAISE(FS_EIO);

        /* Copy entry into caller buffer. */
        dirp->d_ino = ent.d_ino;
        dirp->d_off = off;
        dirp->d_reclen = sizeof(struct dirent);
        dirp->d_type = ent.d_type;
        strlcpy(dirp->d_name, ent.d_name, sizeof(dirp->d_name));

        off += sizeof(struct dirent);
        remaining -= sizeof(struct dirent);

        dirp++;
    }

    *ret = off;

done:
    return err;
}

fs_errno_t fs_access(const char* pathname, int mode, int* ret)
{
    fs_errno_t err = 0;
    fs_t* fs;
    char suffix[FS_PATH_MAX];
    fs_stat_t stat;
    char real_path[FS_PATH_MAX];

    if (ret)
        *ret = -1;

    memset(&stat, 0, sizeof(stat));

    if (!pathname || !ret)
        RAISE(FS_EINVAL);

    CHECK(_realpath(pathname, real_path));

    if (!(fs = fs_lookup(real_path, suffix)))
        RAISE(FS_ENOENT);

    CHECK(fs->fs_stat(fs, suffix, &stat));

    /* ATTN: all accesses possible currently. */

    *ret = 0;

done:
    return err;
}

fs_errno_t fs_getcwd(char* buf, unsigned long size, int* ret)
{
    fs_errno_t err = 0;
    size_t n;

    if (ret)
        *ret = -1;

    if (!buf || !ret)
        RAISE(FS_EINVAL);

    pthread_spin_lock(&lock);
    n = strlcpy(buf, _cwd, size);
    pthread_spin_unlock(&lock);

    if (n >= size)
        RAISE(FS_ERANGE);

    *ret = n + 1;

done:
    return err;
}

fs_errno_t fs_chdir(const char* path, int* ret)
{
    fs_errno_t err = 0;
    size_t n;

    if (ret)
        *ret = -1;

    if (!path || !ret)
        RAISE(FS_EINVAL);

    pthread_spin_lock(&lock);
    n = strlcpy(_cwd, path, FS_PATH_MAX);
    pthread_spin_unlock(&lock);

    if (n >= FS_PATH_MAX)
    {
        pthread_spin_unlock(&lock);
        RAISE(FS_ENAMETOOLONG);
    }

    *ret = 0;

done:
    return err;
}

fs_errno_t fs_opendir(const char* name, DIR** dir_out)
{
    fs_errno_t err = 0;
    fs_t* fs = NULL;
    fs_dir_t* dir = NULL;
    char suffix[FS_PATH_MAX];
    char real_path[FS_PATH_MAX];
    dir_handle_t* h = NULL;

    if (dir_out)
        *dir_out = NULL;

    if (!name || !dir_out)
        RAISE(FS_EINVAL);

    CHECK(_realpath(name, real_path));

    if (!(fs = fs_lookup(real_path, suffix)))
        RAISE(FS_ENOENT);

    if (!(h = calloc(1, sizeof(dir_handle_t))))
        RAISE(FS_ENOMEM);

    CHECK(fs->fs_opendir(fs, suffix, &dir));

    h->magic = DIR_HANDLE_MAGIC;
    h->fs = fs;
    h->dir = dir;

    *dir_out = (DIR*)h;
    h = NULL;

done:

    if (h)
        free(h);

    return err;
}

fs_errno_t fs_readdir(DIR* dirp, struct dirent** entry_out)
{
    fs_errno_t err = 0;
    dir_handle_t* h = (dir_handle_t*)dirp;
    fs_dirent_t* dirent;
    struct dirent* entry;

    if (entry_out)
        *entry_out = NULL;

    if (!dirp || !entry_out || h->magic != DIR_HANDLE_MAGIC)
        RAISE(FS_EINVAL);

    CHECK(h->fs->fs_readdir(h->dir, &dirent));

    if (!dirent)
        goto done;

    entry = &h->entry;
    entry->d_ino = dirent->d_ino;
    entry->d_off = dirent->d_off;
    entry->d_reclen = dirent->d_reclen;
    entry->d_type = dirent->d_type;
    strlcpy(entry->d_name, dirent->d_name, sizeof(entry->d_name));

    *entry_out = entry;

done:
    return err;
}

fs_errno_t fs_closedir(DIR* dirp, int* ret)
{
    fs_errno_t err = 0;
    dir_handle_t* h = (dir_handle_t*)dirp;

    if (ret)
        *ret = -1;

    if (!dirp || !ret || h->magic != DIR_HANDLE_MAGIC)
        RAISE(FS_EINVAL);

    CHECK(h->fs->fs_closedir(h->dir));

    memset(h, 0, sizeof(dir_handle_t));
    free(h);

    *ret = 0;

done:
    return err;
}
