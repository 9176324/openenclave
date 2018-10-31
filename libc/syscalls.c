// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#define __OE_NEED_TIME_CALLS
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <openenclave/enclave.h>
#include <openenclave/internal/calls.h>
#include <openenclave/internal/enclavelibc.h>
#include <openenclave/internal/print.h>
#include <openenclave/internal/syscall.h>
#include <openenclave/internal/thread.h>
#include <openenclave/internal/time.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <time.h>
#include <time.h>
#include <unistd.h>
#include "fs/fs.h"

static oe_syscall_hook_t _hook;
static oe_spinlock_t _lock;

static const uint64_t _SEC_TO_MSEC = 1000UL;
static const uint64_t _MSEC_TO_USEC = 1000UL;
static const uint64_t _MSEC_TO_NSEC = 1000000UL;

#define MAX_FILES 1024

/* Offset to account for stdin=0, stdout=1, stderr=2. */
#define FD_OFFSET 3

typedef struct _file_entry
{
    fs_t* fs;
    fs_file_t* file;
}
file_entry_t;

static file_entry_t _file_entries[MAX_FILES];

static size_t _assign_file_entry()
{
    for (size_t i = 0; i < MAX_FILES; i++)
    {
        if (_file_entries[i].fs == NULL && _file_entries[i].file == NULL)
            return i;
    }

    return (size_t)-1;
}

static long
_syscall_open(long n, long x1, long x2, long x3, long x4, long x5, long x6)
{
    const char* filename = (const char*)x1;
    int flags = (int)x2;
    int mode = (int)x3;
    fs_t* fs = NULL;

    /* Open the file. */
    {
        char suffix[FS_PATH_MAX];

        if ((fs = fs_lookup(filename, suffix)))
        {
            fs_file_t* file;
            fs_errno_t err;
            size_t index;

            if ((index = _assign_file_entry()) == (size_t)-1)
                return -1;

            if ((err = fs->fs_open(fs, suffix, flags, mode, &file)) != 0)
                return -1;

            _file_entries[index].fs = fs;
            _file_entries[index].file = file;

            return index + FD_OFFSET;
        }
    }

    if (flags == O_WRONLY)
        return STDOUT_FILENO;

    return -1;
}

static long _syscall_close(long n, long x1, ...)
{
    int fd = (int)x1;

    if (fd >= FD_OFFSET)
    {
        const size_t index = fd - FD_OFFSET;
        int ret = 0;

        file_entry_t* entry = &_file_entries[index];

        if (!entry->fs || !entry->file)
            return -1;

        if (entry->fs->fs_close(entry->file) != 0)
            ret = -1;

        memset(&_file_entries[index], 0, sizeof(_file_entries));

        return ret;
    }

    /* required by mbedtls */
    return 0;
}

static long _syscall_mmap(long n, ...)
{
    /* Always fail */
    return EPERM;
}

static long _syscall_readv(long num, long x1, long x2, long  x3, ...)
{
    int fd = (int)x1;
    const struct iovec* iov = (const struct iovec*)x2;
    int iovcnt = (int)x3;

    (void)fd;
    (void)iov;
    (void)iovcnt;

    if (fd >= FD_OFFSET)
    {
        const size_t index = fd - FD_OFFSET;
        fs_t* fs = _file_entries[index].fs;
        fs_file_t* file = _file_entries[index].file;
        int32_t ret = 0;

        if (!fs || !file)
            return -1;

        for (int i = 0; i < iovcnt; i++)
        {
            const struct iovec* p = &iov[i];
            int32_t n;
            fs_errno_t err = fs->fs_read(file, p->iov_base, p->iov_len, &n);

            if (err != OE_EOK)
                break;

            ret += n;

            if (n < iov->iov_len)
                break;
        }

        return ret;
    }

    /* required by mbedtls */

    /* return zero-bytes read */
    return 0;
}

static long
_syscall_ioctl(long n, long x1, long x2, long x3, long x4, long x5, long x6)
{
    int fd = (int)x1;

    /* only allow ioctl() on these descriptors */
    if (fd != STDIN_FILENO && fd != STDOUT_FILENO && fd != STDERR_FILENO)
        abort();

    return 0;
}

static long
_syscall_writev(long n, long x1, long x2, long x3, long x4, long x5, long x6)
{
    int fd = (int)x1;
    const struct iovec* iov = (const struct iovec*)x2;
    unsigned long iovcnt = (unsigned long)x3;
    long ret = 0;
    int device;

    /* Allow writing only to stdout and stderr */
    switch (fd)
    {
        case STDOUT_FILENO:
        {
            device = 0;
            break;
        }
        case STDERR_FILENO:
        {
            device = 1;
            break;
        }
        default:
        {
            abort();
        }
    }

    for (unsigned long i = 0; i < iovcnt; i++)
    {
        oe_host_write(device, iov[i].iov_base, iov[i].iov_len);
        ret += iov[i].iov_len;
    }

    return ret;
}

static long _syscall_clock_gettime(long n, long x1, long x2)
{
    clockid_t clk_id = (clockid_t)x1;
    struct timespec* tp = (struct timespec*)x2;
    int ret = -1;
    uint64_t msec;

    if (!tp)
        goto done;

    if (clk_id != CLOCK_REALTIME)
    {
        /* Only supporting CLOCK_REALTIME */
        oe_assert("clock_gettime(): panic" == NULL);
        goto done;
    }

    if ((msec = oe_get_time()) == (uint64_t)-1)
        goto done;

    tp->tv_sec = msec / _SEC_TO_MSEC;
    tp->tv_nsec = (msec % _SEC_TO_MSEC) * _MSEC_TO_NSEC;

    ret = 0;

done:

    return ret;
}

static long _syscall_gettimeofday(long n, long x1, long x2)
{
    struct timeval* tv = (struct timeval*)x1;
    void* tz = (void*)x2;
    int ret = -1;
    uint64_t msec;

    if (tv)
        oe_memset(tv, 0, sizeof(struct timeval));

    if (tz)
        oe_memset(tz, 0, sizeof(struct timezone));

    if (!tv)
        goto done;

    if ((msec = oe_get_time()) == (uint64_t)-1)
        goto done;

    tv->tv_sec = msec / _SEC_TO_MSEC;
    tv->tv_usec = msec % _MSEC_TO_USEC;

    ret = 0;

done:
    return ret;
}

static long _syscall_nanosleep(long n, long x1, long x2)
{
    const struct timespec* req = (struct timespec*)x1;
    struct timespec* rem = (struct timespec*)x2;
    size_t ret = -1;
    uint64_t milliseconds = 0;

    if (rem)
        oe_memset(rem, 0, sizeof(*rem));

    if (!req)
        goto done;

    /* Convert timespec to milliseconds */
    milliseconds += req->tv_sec * 1000UL;
    milliseconds += req->tv_nsec / 1000000UL;

    /* Perform OCALL */
    ret = oe_sleep(milliseconds);

done:

    return ret;
}

/* Intercept __syscalls() from MUSL */
long __syscall(long n, long x1, long x2, long x3, long x4, long x5, long x6)
{
    oe_spin_lock(&_lock);
    oe_syscall_hook_t hook = _hook;
    oe_spin_unlock(&_lock);

    /* Invoke the syscall hook if any */
    if (hook)
    {
        long ret = -1;

        if (hook(n, x1, x2, x3, x4, x5, x6, &ret) == OE_OK)
        {
            /* The hook handled the syscall */
            return ret;
        }

        /* The hook ignored the syscall so fall through */
    }

    switch (n)
    {
        case SYS_nanosleep:
            return _syscall_nanosleep(n, x1, x2);
        case SYS_gettimeofday:
            return _syscall_gettimeofday(n, x1, x2);
        case SYS_clock_gettime:
            return _syscall_clock_gettime(n, x1, x2);
        case SYS_writev:
            return _syscall_writev(n, x1, x2, x3, x4, x5, x6);
        case SYS_ioctl:
            return _syscall_ioctl(n, x1, x2, x3, x4, x5, x6);
        case SYS_open:
            return _syscall_open(n, x1, x2, x3, x4, x5, x6);
        case SYS_close:
            return _syscall_close(n, x1, x2, x3, x4, x5, x6);
        case SYS_mmap:
            return _syscall_mmap(n, x1, x2, x3, x4, x5, x6);
        case SYS_readv:
            return _syscall_readv(n, x1, x2, x3, x4, x5, x6);
        default:
        {
            /* All other MUSL-initiated syscalls are aborted. */
            fprintf(stderr, "error: __syscall(): n=%lu\n", n);
            abort();
            return 0;
        }
    }

    return 0;
}

/* Intercept __syscalls_cp() from MUSL */
long __syscall_cp(long n, long x1, long x2, long x3, long x4, long x5, long x6)
{
    return __syscall(n, x1, x2, x3, x4, x5, x6);
}

long syscall(long number, ...)
{
    va_list ap;

    va_start(ap, number);
    long x1 = va_arg(ap, long);
    long x2 = va_arg(ap, long);
    long x3 = va_arg(ap, long);
    long x4 = va_arg(ap, long);
    long x5 = va_arg(ap, long);
    long x6 = va_arg(ap, long);
    long ret = __syscall(number, x1, x2, x3, x4, x5, x6);
    va_end(ap);

    return ret;
}

void oe_register_syscall_hook(oe_syscall_hook_t hook)
{
    oe_spin_lock(&_lock);
    _hook = hook;
    oe_spin_unlock(&_lock);
}
