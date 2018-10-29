/* Copyright (c) Microsoft Corporation. All rights reserved. */
/* Licensed under the MIT License. */
#include <stdio.h>
#include <string.h>
#include <direct.h>
#include <sys/stat.h>
#include <stdint.h>
#include <errno.h>
#include "tcps_u.h"
#include "../TcpsCalls_u.h"

stat64i32_Result
SGX_CDECL
ocall_stat64i32(
    buffer256 path)
{
    stat64i32_Result result;
    Tcps_StatusCode uStatus = Tcps_Good;
    if (_stat64i32(path.buffer, (struct _stat64i32*)&result.buffer) != 0) {
        result.status = Tcps_Bad;
    }
    result.status = uStatus;
    return result;
}
