// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#ifndef _OE_FS_H
#define _OE_FS_H

#include <openenclave/internal/device.h>

OE_EXTERNC_BEGIN

#define OE_FLAG_NONE 0
#define OE_FLAG_MKFS 1
#define OE_FLAG_CRYPTO 2
#define OE_KEY_SIZE 32

#define OE_DT_UNKNOWN 0
#define OE_DT_FIFO 1 /* unused */
#define OE_DT_CHR 2  /* unused */
#define OE_DT_DIR 4
#define OE_DT_BLK 6 /* unused */
#define OE_DT_REG 8
#define OE_DT_LNK 10  /* unused */
#define OE_DT_SOCK 12 /* unused */
#define OE_DT_WHT 14  /* unused */

#define OE_S_IFSOCK 0xC000
#define OE_S_IFLNK 0xA000
#define OE_S_IFREG 0x8000
#define OE_S_IFBLK 0x6000
#define OE_S_IFDIR 0x4000
#define OE_S_IFCHR 0x2000
#define OE_S_IFIFO 0x1000
#define OE_S_ISUID 0x0800
#define OE_S_ISGID 0x0400
#define OE_S_ISVTX 0x0200
#define OE_S_IRUSR 0x0100
#define OE_S_IWUSR 0x0080
#define OE_S_IXUSR 0x0040
#define OE_S_IRGRP 0x0020
#define OE_S_IWGRP 0x0010
#define OE_S_IXGRP 0x0008
#define OE_S_IROTH 0x0004
#define OE_S_IWOTH 0x0002
#define OE_S_IXOTH 0x0001
#define OE_S_IRWXUSR (OE_S_IRUSR | OE_S_IWUSR | OE_S_IXUSR)
#define OE_S_IRWXGRP (OE_S_IRGRP | OE_S_IWGRP | OE_S_IXGRP)
#define OE_S_IRWXOTH (OE_S_IROTH | OE_S_IWOTH | OE_S_IXOTH)
#define OE_S_IRWXALL (OE_S_IRWXUSR | OE_S_IRWXGRP | OE_S_IRWXOTH)
#define OE_S_IRWUSR (OE_S_IRUSR | OE_S_IWUSR)
#define OE_S_IRWGRP (OE_S_IRGRP | OE_S_IWGRP)
#define OE_S_IRWOTH (OE_S_IROTH | OE_S_IWOTH)
#define OE_S_IRWALL (OE_S_IRWUSR | OE_S_IRWGRP | OE_S_IRWOTH)
#define OE_S_REG_DEFAULT (OE_S_IFREG | OE_S_IRWALL)
#define OE_S_DIR_DEFAULT (OE_S_IFDIR | OE_S_IRWXALL)

// clang-format off
#define OE_O_RDONLY    000000000
#define OE_O_WRONLY    000000001
#define OE_O_RDWR      000000002
#define OE_O_CREAT     000000100
#define OE_O_EXCL      000000200
#define OE_O_NOCTTY    000000400
#define OE_O_TRUNC     000001000
#define OE_O_APPEND    000002000
#define OE_O_NONBLOCK  000004000
#define OE_O_DSYNC     000010000
#define OE_O_SYNC      004010000
#define OE_O_RSYNC     004010000
#define OE_O_DIRECTORY 000200000
#define OE_O_NOFOLLOW  000400000
#define OE_O_CLOEXEC   002000000
#define OE_O_ASYNC     000020000
#define OE_O_DIRECT    000040000
#define OE_O_LARGEFILE 000000000
#define OE_O_NOATIME   001000000
#define OE_O_PATH      010000000
#define OE_O_TMPFILE   020200000
#define OE_O_NDELAY    O_NONBLOCK
// clang-format on

#define OE_SEEK_SET 0
#define OE_SEEK_CUR 1
#define OE_SEEK_END 2

typedef uint32_t oe_mode_t;
typedef int64_t oe_off_t;
typedef struct _oe_file oe_file_t;
typedef struct _oe_dir oe_dir_t;
typedef struct _oe_fs oe_fs_t;

struct oe_dirent
{
    uint32_t d_ino;
    uint32_t d_off;
    uint16_t d_reclen;
    uint8_t d_type;
    char d_name[OE_PATH_MAX];
    uint8_t __d_reserved;
};

typedef struct _oe_timespec
{
    long tv_sec;
    long tv_nsec;
} oe_timespec_t;

struct oe_stat
{
    uint32_t st_dev;
    uint32_t st_ino;
    uint16_t st_mode;
    uint16_t __st_padding1;
    uint32_t st_nlink;
    uint16_t st_uid;
    uint16_t st_gid;
    uint32_t st_rdev;
    uint32_t st_size;
    uint32_t st_blksize;
    uint32_t st_blocks;
    uint32_t __st_padding2;
    oe_timespec_t st_atim;
    oe_timespec_t st_mtim;
    oe_timespec_t st_ctim;
};

struct _oe_file
{
    oe_device_t device;

    oe_off_t (*lseek)(int fd, oe_off_t offset, int whence);

    int (*fcntl)(int fd, int cmd, ...);
};

struct _oe_dir
{
    struct oe_dirent* (*readdir)(oe_dir_t *dirp);

    int (*closedir)(oe_dir_t *dirp);
};

struct _oe_fs
{
    /* Decrement the internal reference count and release if zero. */
    void (*release)(oe_fs_t* fs);

    /* Increment the internal reference count. */
    void (*add_ref)(oe_fs_t* fs);

    int (*open)(oe_fs_t* fs, const char *pathname, int flags, oe_mode_t mode);

    oe_dir_t* (*opendir)(oe_fs_t* fs, const char* path);

    int (*stat)(oe_fs_t* fs, const char *pathname, struct oe_stat *buf);

    int (*link)(oe_fs_t* fs, const char *oldpath, const char *newpath);

    int (*unlink)(oe_fs_t* fs, const char *pathname);

    int (*rename)(oe_fs_t* fs, const char *oldpath, const char *newpath);

    int (*truncate)(oe_fs_t* fs, const char *path, oe_off_t length);

    int (*mkdir)(oe_fs_t* fs, const char *pathname, oe_mode_t mode);

    int (*rmdir)(oe_fs_t* fs, const char *pathname);
};

OE_EXTERNC_END

#endif // _OE_FS_H
