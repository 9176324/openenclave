#ifndef _OE_FSINTERNAL_H
#define _OE_FSINTERNAL_H

#include <dirent.h>
#include <openenclave/bits/defs.h>
#include <openenclave/bits/types.h>
#include <stdio.h>

OE_EXTERNC_BEGIN

typedef struct _IO_FILE FILE;
typedef struct __dirstream DIR;

#define OE_FILE_MAGIC 0x0EF55FE0

struct _IO_FILE
{
    /* Contains OE_FILE_MAGIC. */
    uint32_t magic;

    /* Padding to prevent overlap with MUSL _IO_FILE struct. */
    uint8_t padding[252];

    int32_t (*f_fclose)(FILE* file);

    size_t (*f_fread)(void* ptr, size_t size, size_t nmemb, FILE* file);

    size_t (*f_fwrite)(const void* ptr, size_t size, size_t nmemb, FILE* file);

    int64_t (*f_ftell)(FILE* file);

    int32_t (*f_fseek)(FILE* file, int64_t offset, int whence);

    int32_t (*f_fflush)(FILE* file);

    int32_t (*f_ferror)(FILE* file);

    int32_t (*f_feof)(FILE* file);

    int32_t (*f_clearerr)(FILE* file);
};

struct __dirstream
{
    int32_t (
        *d_readdir)(DIR* dir, struct dirent* entry, struct dirent** result);

    int32_t (*d_closedir)(DIR* dir);
};

OE_EXTERNC_END

#endif /* _OE_FSINTERNAL_H */
