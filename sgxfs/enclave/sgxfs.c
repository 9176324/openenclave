// clang-format off
#include "linux-sgx/common/inc/sgx_tprotected_fs.h"
#undef FILENAME_MAX
#undef FOPEN_MAX
// clang-format on

#include <stdlib.h>
#include <stdio.h>
#include "../common/sgxfs.h"
#include <openenclave/internal/fsinternal.h>

typedef struct _file
{
    oe_file_t base;
    SGX_FILE* sgx_file;
}
file_t;

static int32_t _f_fclose(oe_file_t* base)
{
    int ret = -1;
    file_t* file = (file_t*)base;

    if (!file || !file->sgx_file)
        goto done;

    if (sgx_fclose(file->sgx_file) != 0)
        goto done;

    free(file);

    ret = 0;

done:
    return ret;
}

static size_t _f_fread(void* ptr, size_t size, size_t nmemb, oe_file_t* base)
{
    size_t ret = 0;
    file_t* file = (file_t*)base;

    if (!ptr || !file || !file->sgx_file)
        goto done;

    ret = sgx_fread(ptr, size, nmemb, file->sgx_file);

done:

    return ret;
}

static size_t _f_fwrite(
    const void* ptr, size_t size, size_t nmemb, oe_file_t* base)
{
    size_t ret = 0;
    file_t* file = (file_t*)base;

    if (!ptr || !file || !file->sgx_file)
        goto done;

    ret = sgx_fwrite(ptr, size, nmemb, file->sgx_file);

done:

    return ret;
}

static int64_t _f_ftell(oe_file_t* base)
{
    int64_t ret = -1;
    file_t* file = (file_t*)base;

    if (!file || !file->sgx_file)
        goto done;

    ret = sgx_fclose(file->sgx_file);

done:
    return ret;
}

static int32_t _f_fseek(oe_file_t* base, int64_t offset, int whence)
{
    int32_t ret = -1;
    file_t* file = (file_t*)base;

    if (!file || !file->sgx_file)
        goto done;

    ret = sgx_fseek(file->sgx_file, offset, whence);

done:
    return ret;
}

static int32_t _f_fflush(oe_file_t* base)
{
    int ret = -1;
    file_t* file = (file_t*)base;

    if (!file || !file->sgx_file)
        goto done;

    ret = sgx_fflush(file->sgx_file);

done:
    return ret;
}

static int32_t _f_ferror(oe_file_t* base)
{
    int ret = -1;
    file_t* file = (file_t*)base;

    if (!file || !file->sgx_file)
        goto done;

    ret = sgx_ferror(file->sgx_file);

done:
    return ret;
}

static int32_t _f_feof(oe_file_t* base)
{
    int ret = -1;
    file_t* file = (file_t*)base;

    if (!file || !file->sgx_file)
        goto done;

    ret = sgx_feof(file->sgx_file);

done:
    return ret;
}

static int32_t _f_clearerr(oe_file_t* base)
{
    int ret = -1;
    file_t* file = (file_t*)base;

    if (!file || !file->sgx_file)
        goto done;

    sgx_clearerr(file->sgx_file);
    ret = 0;

done:
    return ret;
}

static oe_file_t* _fs_fopen(
    oe_fs_t* fs,
    const char* path,
    const char* mode,
    const void* args)
{
    oe_file_t* ret = NULL;
    file_t* file = NULL;

    if (!path || !mode)
        goto done;

    if (!(file = calloc(1, sizeof(file_t))))
        goto done;
    
    if (args)
    {
        if (!(file->sgx_file = sgx_fopen(path, mode, args)))
            return NULL;
    }
    else
    {
        if (!(file->sgx_file = sgx_fopen_auto_key(path, mode)))
            return NULL;
    }
    
    file->base.f_fclose = _f_fclose;
    file->base.f_fread = _f_fread;
    file->base.f_fwrite = _f_fwrite;
    file->base.f_ftell = _f_ftell;
    file->base.f_fseek = _f_fseek;
    file->base.f_fflush = _f_fflush;
    file->base.f_ferror = _f_ferror;
    file->base.f_feof = _f_feof;
    file->base.f_clearerr = _f_clearerr;

    ret = &file->base;
    file = NULL;

done:

    if (file)
        free(file);

    return ret;
}

static int32_t _fs_release(oe_fs_t* fs)
{
    uint32_t ret = -1;
    
    if (!fs)
        goto done;

    ret = 0;

done:
    return ret;
}

oe_fs_t oe_sgxfs =
{
    .fs_fopen = _fs_fopen,
    .fs_release = _fs_release,
};
