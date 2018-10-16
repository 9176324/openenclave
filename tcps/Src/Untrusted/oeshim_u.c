/* Copyright (c) Microsoft Corporation. All rights reserved. */
/* Licensed under the MIT License. */
#include <openenclave/bits/types.h>
#include <openenclave/host.h>
#include <tcps_u.h>
#include <sgx.h>
#include <Windows.h>
#include "TcpsCalls_u.h"
#include "../oeresult.h"

typedef struct {
    size_t nr_ocall;
    oe_call_t* call_addr;
} ocall_table_v2_t;

ocall_table_v2_t g_ocall_table_v2 = { 0 };

oe_result_t oe_create_enclave(
    _In_z_ const char* path,
    _In_ oe_enclave_type_t type,
    _In_ uint32_t flags,
    _In_reads_bytes_(configSize) const void* config,
    _In_ uint32_t configSize,
    _Out_ oe_enclave_t** enclave)
{
    *enclave = NULL;

    TCPS_UNUSED(type);
    TCPS_UNUSED(config);
    TCPS_UNUSED(configSize);

    // Load the enclave.
    sgx_enclave_id_t eid;
    Tcps_StatusCode uStatus = Tcps_CreateTA(path, flags, &eid);
    if (Tcps_IsBad(uStatus)) {
        return OE_FAILURE;
    }

    *enclave = (oe_enclave_t*)eid;
    return OE_OK;
}

oe_result_t oe_create_enclave_v2(
    const char* path,
    oe_enclave_type_t type,
    uint32_t flags,
    const void* config,
    uint32_t config_size,
    void (**ocall_table)(void*), /* TODO: type of this argument is still being discussed */
    uint32_t ocall_table_size,
    oe_enclave_t** enclave)
{
    TCPS_UNUSED(ocall_table_size);

    g_ocall_table_v2.nr_ocall = ocall_table_size;
    g_ocall_table_v2.call_addr = (oe_call_t*)ocall_table;
    
    return oe_create_enclave(path, type, flags, config, config_size, enclave);
}

oe_result_t oe_call_enclave(oe_enclave_t* enclave, const char* func, void* args)
{
    // This API is deprecated.
    return OE_FAILURE;
}

oe_result_t oe_ecall(oe_enclave_t* enclave, uint16_t func, uint64_t argIn, uint64_t* argOut)
{
    // This API is deprecated.
    return OE_FAILURE;
}

oe_result_t oe_terminate_enclave(_In_ oe_enclave_t* enclave)
{
    sgx_enclave_id_t eid = (sgx_enclave_id_t)enclave;
    Tcps_StatusCode uStatus = Tcps_DestroyTA(eid);
    return Tcps_IsBad(uStatus) ? OE_FAILURE : OE_OK;
}

const char* oe_result_str(_In_ oe_result_t result)
{
    static char message[80];
    sprintf_s(message, sizeof(message), "Error %d", result);
    return message;
}

void* ocall_malloc(_In_ size_t size)
{
    return malloc(size);
}

void* ocall_realloc(_In_ void* ptr, _In_ size_t size)
{
    return realloc(ptr, size);
}

void* ocall_calloc(_In_ size_t nmemb, _In_ size_t size)
{
    return calloc(nmemb, size);
}

void ocall_free(_In_ void* ptr)
{
    free(ptr);
}

void ocall_CopyReeMemoryFromBufferChunk(
    _In_ void* ptr,
    _In_ BufferChunk chunk)
{
    memcpy(ptr, chunk.buffer, chunk.size);
}

oe_result_t oe_get_report(
    _In_ oe_enclave_t* enclave,
    _In_ uint32_t flags,
    _In_reads_opt_(opt_params_size) const void* opt_params,
    _In_ size_t opt_params_size,
    _Out_ uint8_t* report_buffer,
    _Out_ size_t* report_buffer_size)
{
    GetReport_Result result;
    buffer1024 optParamsBuffer;
    sgx_enclave_id_t eid = (sgx_enclave_id_t)enclave;
    COPY_BUFFER(optParamsBuffer, opt_params, opt_params_size);
    sgx_status_t sgxStatus = ecall_get_report(eid, &result, flags, optParamsBuffer, opt_params_size);
    oe_result_t oeResult = GetOEResultFromSgxStatus(sgxStatus);
    if (oeResult == OE_OK) {
        *report_buffer_size = result.report_buffer_size;
        memcpy(report_buffer, result.report_buffer, result.report_buffer_size);
    }
    return oeResult;
}

oe_result_t oe_verify_report(
    _In_ oe_enclave_t* enclave,
    _In_reads_(report_size) const uint8_t* report,
    _In_ size_t report_size,
    _Out_opt_ oe_report_t* parsed_report)
{
    buffer1024 reportBuffer;
    oe_result_t oeResult;
    sgx_enclave_id_t eid = (sgx_enclave_id_t)enclave;
    if (parsed_report != NULL) {
        oeResult = oe_parse_report(report, report_size, parsed_report);
        if (oeResult != OE_OK) {
            return oeResult;
        }
    }

    COPY_BUFFER(reportBuffer, report, report_size);
    sgx_status_t sgxStatus = ecall_verify_report(eid, (int*)&oeResult, reportBuffer, report_size);
    if (sgxStatus != SGX_SUCCESS) {
        return GetOEResultFromSgxStatus(sgxStatus);
    }
    return oeResult;
}

callV2_Result ocall_v2(uint32_t func, buffer4096 inBuffer, size_t inBufferSize)
{
    callV2_Result result;

    if (func > g_ocall_table_v2.nr_ocall) {
        result.outBufferSize = 0;
        return result;
    }

    oe_call_t call = g_ocall_table_v2.call_addr[func];
    call(inBuffer.buffer,
        inBufferSize,
        result.outBuffer,
        sizeof(result.outBuffer),
        &result.outBufferSize);

    return result;
}

oe_result_t oe_call_enclave_function( 
    _In_ oe_enclave_t* enclave,
    _In_ uint32_t function_id,
    _In_reads_bytes_(input_buffer_size) void* input_buffer,
    _In_ size_t input_buffer_size,
    _Out_writes_bytes_to_(output_buffer_size, *output_bytes_written) void* output_buffer,
    _In_ size_t output_buffer_size,
    _Out_ size_t* output_bytes_written)
{
    if (output_buffer_size > 4096) {
        return OE_INVALID_PARAMETER;
    }

    callV2_Result* result = malloc(sizeof(*result));
    if (result == NULL) {
        return OE_OUT_OF_MEMORY;
    }

    sgx_enclave_id_t eid = (sgx_enclave_id_t)enclave;
    buffer4096 inBufferStruct;
    COPY_BUFFER(inBufferStruct, input_buffer, input_buffer_size);
    sgx_status_t sgxStatus = ecall_v2(eid, result, function_id, inBufferStruct, input_buffer_size);
    if (sgxStatus == SGX_SUCCESS) {
        memcpy(output_buffer, result->outBuffer, result->outBufferSize);
        *output_bytes_written = result->outBufferSize;
    }
    oe_result_t oeResult = GetOEResultFromSgxStatus(sgxStatus);
    free(result);
    return oeResult;
}
