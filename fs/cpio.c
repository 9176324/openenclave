// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#define _GNU_SOURCE
#include "cpio.h"
#include <limits.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#define FS_CPIO_MODE_IFMT 00170000
#define FS_CPIO_MODE_IFSOCK 0140000
#define FS_CPIO_MODE_IFLNK 0120000
#define FS_CPIO_MODE_IFREG 0100000
#define FS_CPIO_MODE_IFBLK 0060000
#define FS_CPIO_MODE_IFDIR 0040000
#define FS_CPIO_MODE_IFCHR 0020000
#define FS_CPIO_MODE_IFIFO 0010000
#define FS_CPIO_MODE_ISUID 0004000
#define FS_CPIO_MODE_ISGID 0002000
#define FS_CPIO_MODE_ISVTX 0001000

#define FS_CPIO_MODE_IRWXU 00700
#define FS_CPIO_MODE_IRUSR 00400
#define FS_CPIO_MODE_IWUSR 00200
#define FS_CPIO_MODE_IXUSR 00100

#define FS_CPIO_MODE_IRWXG 00070
#define FS_CPIO_MODE_IRGRP 00040
#define FS_CPIO_MODE_IWGRP 00020
#define FS_CPIO_MODE_IXGRP 00010

#define FS_CPIO_MODE_IRWXO 00007
#define FS_CPIO_MODE_IROTH 00004
#define FS_CPIO_MODE_IWOTH 00002
#define FS_CPIO_MODE_IXOTH 00001

typedef struct _cpio_header
{
    char magic[6];
    char ino[8];
    char mode[8];
    char uid[8];
    char gid[8];
    char nlink[8];
    char mtime[8];
    char filesize[8];
    char devmajor[8];
    char devminor[8];
    char rdevmajor[8];
    char rdevminor[8];
    char namesize[8];
    char check[8];
} cpio_header_t;

struct _fs_cpio
{
    FILE* stream;
    cpio_header_t header;
    size_t entry_size;
    long eof_offset;
    long offset;
};

static bool _valid_header(const cpio_header_t* header)
{
    return memcmp(header->magic, "070701", 6) == 0;
}

static ssize_t _hex_to_ssize(const char* str, size_t len)
{
    const char* p;
    ssize_t r = 1;
    ssize_t x = 0;

    for (p = str + len; p != str; p--)
    {
        ssize_t xdigit = p[-1];
        ssize_t d;

        if (xdigit >= '0' && xdigit <= '9')
        {
            d = xdigit - '0';
        }
        else if (xdigit >= 'A' && xdigit <= 'F')
        {
            d = (xdigit - 'A') + 10;
        }
        else
            return -1;

        x += r * d;
        r *= 16;
    }

    return x;
}

static size_t _round_to_multiple(size_t x, size_t m)
{
    return (size_t)((x + (m - 1)) / m * m);
}

static ssize_t _get_mode(const cpio_header_t* header)
{
    return _hex_to_ssize(header->mode, 8);
}

static ssize_t _get_filesize(const cpio_header_t* header)
{
    return _hex_to_ssize(header->filesize, 8);
}

static ssize_t _get_namesize(const cpio_header_t* header)
{
    return _hex_to_ssize(header->namesize, 8);
}

static int _skip_padding(FILE* stream)
{
    int ret = -1;
    long pos;
    long new_pos;

    if ((pos = ftell(stream)) < 0)
        goto done;

    new_pos = _round_to_multiple(pos, 4);

    if (new_pos != pos && fseek(stream, new_pos, SEEK_SET) != 0)
        goto done;

    ret = 0;
    goto done;

done:
    return ret;
}

fs_cpio_t* fs_cpio_open(const char* path)
{
    fs_cpio_t* ret = NULL;
    fs_cpio_t* cpio = NULL;
    FILE* stream = NULL;

    if (!path)
        goto done;

    if (!(cpio = calloc(1, sizeof(fs_cpio_t))))
        goto done;

    if (!(stream = fopen(path, "rb")))
        goto done;

    cpio->stream = stream;
    stream = NULL;

    ret = cpio;
    cpio = NULL;

done:

    if (stream)
        fclose(stream);

    if (cpio)
        free(cpio);

    return ret;
}

int fs_cpio_close(fs_cpio_t* cpio)
{
    int ret = -1;

    if (!cpio || !cpio->stream)
        goto done;

    fclose(cpio->stream);
    free(cpio);
    memset(cpio, 0, sizeof(fs_cpio_t));

    ret = 0;

done:
    return ret;
}

/* Read next entry: HEADER + NAME + FILEDATA + PADDING */
int fs_cpio_next(fs_cpio_t* cpio, fs_cpio_entry_t* entry_out)
{
    int ret = -1;
    cpio_header_t header;
    fs_cpio_entry_t entry;
    ssize_t r;
    long file_offset;
    size_t namesize;

    if (entry_out)
        memset(entry_out, 0, sizeof(fs_cpio_entry_t));

    if (!cpio || !cpio->stream)
        goto done;

    /* Set the position to the next entry. */
    if (fseek(cpio->stream, cpio->offset, SEEK_SET) != 0)
        goto done;

    if (fread(&header, 1, sizeof(header), cpio->stream) != sizeof(header))
        goto done;

    if (!_valid_header(&header))
        goto done;

    /* Get the file size. */
    {
        if ((r = _get_filesize(&header)) < 0)
            goto done;

        entry.size = (size_t)r;
    }

    /* Get the file mode. */
    {
        if ((r = _get_mode(&header)) < 0 || r >= UINT32_MAX)
            goto done;

        entry.mode = (uint32_t)r;
    }

    /* Get the name size. */
    {
        if ((r = _get_namesize(&header)) < 0 || r >= FS_PATH_MAX)
            goto done;

        namesize = (size_t)r;
    }

    /* Read the name. */
    if (fread(&entry.name, 1, namesize, cpio->stream) != namesize)
        goto done;

    /* Skip any padding after the name. */
    if (_skip_padding(cpio->stream) != 0)
        goto done;

    /* Save the file offset. */
    file_offset = ftell(cpio->stream);

    /* Skip over the file data. */
    if (fseek(cpio->stream, entry.size, SEEK_CUR) != 0)
        goto done;

    /* Save the file offset. */
    cpio->eof_offset = ftell(cpio->stream);

    /* Skip any padding after the file data. */
    if (_skip_padding(cpio->stream) != 0)
        goto done;

    /* Save the offset of the next entry. */
    cpio->offset = ftell(cpio->stream);

    /* Rewind to the file offset. */
    if (fseek(cpio->stream, file_offset, SEEK_SET) != 0)
        goto done;

    /* Check for end-of-file. */
    if (strcmp(entry.name, "TRAILER!!!") == 0)
    {
        ret = 0;
        goto done;
    }

    *entry_out = entry;

    ret = 1;

done:
    return ret;
}

ssize_t fs_cpio_read(fs_cpio_t* cpio, void* data, size_t size)
{
    ssize_t ret = -1;
    size_t rem;
    ssize_t n;
    long offset;

    if (!cpio || !cpio->stream || !data)
        goto done;

    offset = ftell(cpio->stream);
    
    if (offset > cpio->eof_offset)
        goto done;

    rem = cpio->eof_offset - offset;

    if (size > rem)
        size = rem;

    if ((n = fread(data, 1, size, cpio->stream)) != size)
        goto done;

    ret = n;

done:
    return ret;
}

int fs_cpio_extract(const char* source, const char* target)
{
    int ret = -1;
    fs_cpio_t* cpio = NULL;
    int r;
    fs_cpio_entry_t entry;
    char path[FS_PATH_MAX];
    FILE* os = NULL;

    if (!source || !target)
        goto done;

    if (!(cpio = fs_cpio_open(source)))
        goto done;

    if (access(target, R_OK) != 0 && mkdir(target, 0766) != 0)
        goto done;

    while ((r = fs_cpio_next(cpio, &entry)) > 0)
    {
        if (strcmp(entry.name, ".") == 0)
            continue;

        strlcpy(path, target, sizeof(path));
        strlcat(path, "/", sizeof(path));
        strlcat(path, entry.name, sizeof(path));

        if (S_ISDIR(entry.mode))
        {
            if (access(path, R_OK) && mkdir(path, entry.mode) != 0)
                goto done;
        }
        else
        {
            char data[512];
            ssize_t n;

            if (!(os = fopen(path, "wb")))
                goto done;

            while ((n = fs_cpio_read(cpio, data, sizeof(data))) > 0)
            {
                if (fwrite(data, 1, n, os) != n)
                    goto done;
            }

            fclose(os);
            os = NULL;
        }
    }

    ret = 0;

done:

    if (cpio)
        fs_cpio_close(cpio);

    if (os)
        fclose(os);

    return ret;
}
