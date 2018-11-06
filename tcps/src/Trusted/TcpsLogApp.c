/* Copyright (c) Microsoft Corporation. All rights reserved. */
/* Licensed under the MIT License. */
#include <stdio.h>
#include <openenclave/enclave.h>
#include "tcps_stdio_t.h"
#include "tcps_time_t.h"
#include <string.h>
#include <cbor.h>
#include "cborhelper.h"

#ifdef USE_OPTEE
#include <optee/tcps_string_optee_t.h>
#endif

#include <TcpsLog.h>
#include <TcpsTls.h>

#ifdef USE_SGX
#include <TcpsLogSgx.h>
#endif

extern TCPS_TA_ID_INFO TestIdentityData;

void* g_TcpsLogPlatHandle = NULL;

#include "TcpsLogApp.h"

#if defined(USE_OPTEE) || defined(USE_SGX)
#include "TcpsLogOcallFile.h" 
#endif

#ifdef USE_OPTEE
TCPS_LOG_OCALL_OBJECT* g_TcpsLogLogObjectOptee = NULL;

#define COOKIE_FILE "LogHash.dat"

static
Tcps_Void
TcpsLogCloseOptee(
    Tcps_Void* a_Handle)
{
    TcpsLogClose(a_Handle);

    if (g_TcpsLogLogObjectOptee != NULL)
    {
        if (g_TcpsLogLogObjectOptee->LogPathPrefix != NULL)
        {
            TCPS_FREE(g_TcpsLogLogObjectOptee->LogPathPrefix);
        }

        TCPS_FREE(g_TcpsLogLogObjectOptee);
        g_TcpsLogLogObjectOptee = NULL;
    }
}

static
Tcps_StatusCode
TcpsLogCounterValidateOptee(
    Tcps_Void* a_Handle,
    const uint8_t* const a_CounterIdBuffer,
    const size_t a_CounterIdBufferSize,
    const uint8_t* const a_CounterValueBuffer,
    const size_t a_CounterValueBufferSize)
{
    TCPS_UNUSED(a_Handle);
    TCPS_UNUSED(a_CounterIdBuffer);
    TCPS_UNUSED(a_CounterIdBufferSize);
    TCPS_UNUSED(a_CounterValueBuffer);
    TCPS_UNUSED(a_CounterValueBufferSize);
    return Tcps_Good;
}

static
Tcps_StatusCode
TcpsLogCounterCreateOptee(
    Tcps_Void* a_Handle,
    uint8_t** const a_CounterIdBuffer,
    size_t* const a_CounterIdBufferSize,
    uint8_t** const a_CounterValueBuffer,
    size_t* const a_CounterValueBufferSize)
{
    if (a_Handle == NULL ||
        a_CounterIdBuffer == NULL ||
        a_CounterIdBufferSize == NULL ||
        a_CounterValueBuffer == NULL ||
        a_CounterValueBufferSize == NULL)
    {
        return Tcps_BadInvalidArgument;
    }

    *a_CounterIdBuffer = NULL;
    *a_CounterIdBufferSize = 0;
    *a_CounterValueBuffer = NULL;
    *a_CounterValueBufferSize = 0;

    return Tcps_Good;
}

static
Tcps_StatusCode
TcpsLogCounterIncrementGetOptee(
    Tcps_Void* a_Handle,
    const uint8_t* const a_CounterIdBuffer,
    const size_t a_CounterIdBufferSize,
    uint8_t** const a_CounterValueBuffer,
    size_t* a_CounterValueBufferSize)
{
    TCPS_UNUSED(a_CounterIdBuffer);
    TCPS_UNUSED(a_CounterIdBufferSize);

    if (a_Handle == NULL ||
        a_CounterValueBuffer == NULL ||
        a_CounterValueBufferSize == NULL)
    {
        return Tcps_BadInvalidArgument;
    }

    *a_CounterValueBuffer = NULL;
    *a_CounterValueBufferSize = 0;

    return Tcps_Good;
}

static
Tcps_StatusCode
TcpsLogCounterRecoverOptee(
    Tcps_Void* a_Handle,
    uint8_t** const a_CounterBuffer,
    size_t* const a_CounterBufferSize,
    const TCPS_IDENTITY_LOG a_LogIdentityLabel)
{
    Tcps_StatusCode status = Tcps_Good;
    FILE* counterFile;
    long int counterFileSize;

    if (a_CounterBuffer == NULL ||
        a_CounterBufferSize == NULL ||
        a_Handle == NULL ||
        a_LogIdentityLabel == NULL)
    {
        return Tcps_BadInvalidArgument;
    }

    *a_CounterBuffer = NULL;

    counterFile = fopen(COOKIE_FILE, "rb");
    if (counterFile == NULL)
    {
        goto Exit;
    }

    if (fseek(counterFile, 0, SEEK_END))
    {
        status = Tcps_Bad;
        goto Exit;
    }

    counterFileSize = ftell(counterFile);
    if (counterFileSize == -1)
    {
        status = Tcps_Bad;
        goto Exit;
    }

    if (fseek(counterFile, 0, SEEK_SET))
    {
        status = Tcps_Bad;
        goto Exit;
    }

    *a_CounterBuffer = TCPS_ALLOC(counterFileSize);
    if (*a_CounterBuffer == NULL)
    {
        status = Tcps_BadOutOfMemory;
        goto Exit;
    }

    *a_CounterBufferSize = fread(*a_CounterBuffer, 1, counterFileSize, counterFile);
    if (*a_CounterBufferSize != (size_t)counterFileSize)
    {
        status = Tcps_Bad;
        goto Exit;
    }

Exit:
    if (status != Tcps_Good)
    {
        if (*a_CounterBuffer != NULL)
        {
            TCPS_FREE(*a_CounterBuffer);
        }
    }

    if (counterFile != NULL)
    {
        fclose(counterFile);
    }

    return status;
}

static
Tcps_StatusCode
TcpsLogCounterWriteOptee(
    Tcps_Void* a_Handle,
    const uint8_t* const a_CounterBuffer,
    const size_t a_CounterBufferSize,
    const TCPS_IDENTITY_LOG a_LogIdentityLabel)
{
    size_t written;
    FILE* counterFile;
    Tcps_StatusCode status = Tcps_Good;

    if (a_Handle == NULL ||
        a_LogIdentityLabel == NULL ||
        a_CounterBuffer == NULL)
    {
        return Tcps_BadInvalidArgument;
    }

    counterFile = fopen(COOKIE_FILE, "w");
    if (counterFile == NULL)
    {
        return Tcps_Bad;
    }

    written = fwrite(a_CounterBuffer, 1, a_CounterBufferSize, counterFile);
    if (written != a_CounterBufferSize)
    {
        status = Tcps_Bad;
        goto Exit;
    }

Exit:
    if (counterFile != NULL)
    {
        fclose(counterFile);
    }
    return status;
}

static
Tcps_StatusCode
TcpsLogInitOptee(
    const TCPS_SHA256_DIGEST a_Seed,
    const TCPS_TA_ID_INFO* const a_IDData,
    const char* const a_LogPathPrefix,
    Tcps_Void** a_Handle)
{
    size_t logPathPrefixSize;
    Tcps_StatusCode status = Tcps_Good;

    if (a_IDData == NULL ||
        a_LogPathPrefix == NULL ||
        a_Seed == NULL ||
        a_Handle == NULL ||
        g_TcpsLogLogObjectOptee != NULL)
    {
        return Tcps_Bad;
    }

    *a_Handle = NULL;

    g_TcpsLogLogObjectOptee = TCPS_ALLOC(sizeof(TCPS_LOG_OCALL_OBJECT));
    if (g_TcpsLogLogObjectOptee == NULL)
    {
        status = Tcps_BadOutOfMemory;
        goto Exit;
    }
    TCPS_ZERO(g_TcpsLogLogObjectOptee, sizeof(TCPS_LOG_OCALL_OBJECT));

    logPathPrefixSize = strlen(a_LogPathPrefix) + 1;
    g_TcpsLogLogObjectOptee->LogPathPrefix = TCPS_ALLOC(logPathPrefixSize);
    if (g_TcpsLogLogObjectOptee->LogPathPrefix == NULL)
    {
        status = Tcps_BadOutOfMemory;
        goto Exit;
    }
    strncpy(g_TcpsLogLogObjectOptee->LogPathPrefix, a_LogPathPrefix, logPathPrefixSize);

    status = TcpsLogInit(
        &a_IDData->CompoundPrivKey,
        &a_IDData->CompoundPubKey,
        (PTCPS_LOG_WRITE)TcpsLogFileWriteEntryOcall,
        true,
        NULL,
        NULL,
        NULL,
        NULL, 
        TcpsLogCounterRecoverOptee,
        TcpsLogCounterWriteOptee,
        TcpsLogCounterValidateOptee,
        TcpsLogCounterCreateOptee,
        TcpsLogCounterIncrementGetOptee,
        _time64,
        a_Seed,
        g_TcpsLogLogObjectOptee,
        a_Handle);
    if (status != Tcps_Good)
    {
        goto Exit;
    }

Exit:
    if (status != Tcps_Good)
    {
        TcpsLogCloseOptee(*a_Handle);
    }

    return status;
}

static
Tcps_StatusCode
TcpsLogEventOpteeId(
    Tcps_Void* a_Handle,
    uint32_t a_EventId,
    bool a_Flush)
{
    return TcpsLogEvent(a_Handle, (const uint8_t*)&a_EventId, sizeof(a_EventId), a_Flush);
}

static
Tcps_StatusCode
TcpsLogEventOptee(
    Tcps_Void* a_Handle,
    const uint8_t* const a_Buffer,
    const size_t a_BufferSize,
    const bool a_Flush)
{
    return TcpsLogEvent(a_Handle, a_Buffer, a_BufferSize, a_Flush);
}
#endif

Tcps_StatusCode
TcpsLogAppInit(
    const char* const a_sLogPrefix)
{
#if defined(USE_SGX) || defined(USE_OPTEE)
    TCPS_SHA256_DIGEST seed = { 0 }; // TODO provision
#else 
    TCPS_UNUSED(a_sLogPrefix);
#endif

    Tcps_StatusCode status = Tcps_BadNotImplemented;
    
#ifdef USE_SGX
    TCPS_LOG_SGX_CONFIGURATION configuration;
    strncpy(configuration.LogPathPrefix, a_sLogPrefix, sizeof(configuration.LogPathPrefix));
    configuration.LogPathPrefix[sizeof(configuration.LogPathPrefix) - 1] = '\0';
    configuration.RemoteType = TCPS_LOG_SGX_REMOTE_TYPE_NONE;

#ifdef TCPS_USE_TRUSTED_TIME
    // To establish the correct base time, we need to provision using untrusted time
    // TODO provision securely
    TcpsLogTimeProvisionSgx(time(NULL));

    configuration.TimeFunc = TcpsLogTrustedTimeSgx;
#else
    configuration.TimeFunc = TcpsLogUntrustedTimeSgx;
#endif

    status = TcpsLogInitSgx(
        seed,
        &TestIdentityData,
        &configuration,
        true,
        &g_TcpsLogPlatHandle);
#elif USE_OPTEE
    status = TcpsLogInitOptee(
        seed,
        &TestIdentityData,
        a_sLogPrefix,
        &g_TcpsLogPlatHandle);
#endif

    if (status != Tcps_Good)
    {
        return Tcps_BadInvalidState;
    }

    return Tcps_Good;
}

Tcps_Void
TcpsLogAppClose(void)
{
    if (g_TcpsLogPlatHandle != NULL)
    {
#ifdef USE_SGX
        TcpsLogCloseSgx(g_TcpsLogPlatHandle);
#elif USE_OPTEE
        TcpsLogCloseOptee(g_TcpsLogPlatHandle);
#endif
        g_TcpsLogPlatHandle = NULL;
    }
}

Tcps_StatusCode
TcpsLogAppEventId(
    const int a_Id)
{
    Tcps_StatusCode status = Tcps_BadNotImplemented;
#ifdef _OE_ENCLAVE_H
    if (g_TcpsLogPlatHandle)
#ifdef USE_SGX
        status = TcpsLogEventSgxId(g_TcpsLogPlatHandle, a_Id, a_Id < 0);
#elif USE_OPTEE
        status = TcpsLogEventOpteeId(g_TcpsLogPlatHandle, a_Id, a_Id < 0);
#endif
    else
        status = Tcps_Bad;
#else
    TCPS_UNUSED(a_Id);
#endif

    return status == Tcps_Good ?
        Tcps_Good :
        Tcps_BadInvalidState;
}

static
Tcps_StatusCode
TcpsLogAppEvent(
    const uint8_t* const a_Buffer,
    const size_t a_BufferSize)
{
    Tcps_StatusCode status = Tcps_BadNotImplemented;
#ifdef _OE_ENCLAVE_H
    if (g_TcpsLogPlatHandle)
#ifdef USE_SGX
        status = TcpsLogEventSgx(g_TcpsLogPlatHandle, a_Buffer, a_BufferSize, false);
#elif USE_OPTEE
        status = TcpsLogEventOptee(g_TcpsLogPlatHandle, a_Buffer, a_BufferSize, false);
#endif
    else
        status = Tcps_Bad;
#else
    TCPS_UNUSED(a_Buffer);
    TCPS_UNUSED(a_BufferSize);
#endif

    return status == Tcps_Good ?
        Tcps_Good :
        Tcps_BadInvalidState;
}

#ifndef __GNUC__
#pragma region SerDe Helpers

#pragma region Encode TCPS_LOG_APP_EVENT
#endif

static
CborError
TcpsCborEncodeAppEvent(
    const TCPS_LOG_APP_EVENT_DATA* const Payload,
    size_t* const BufferSize,
    uint8_t* const Buffer)
/*++

Routine Description:

Parameters:

--*/
{
    CborEncoder encoder, arrayEncoder;
    CborError err = CborNoError;

    if (BufferSize == NULL ||
        (*BufferSize > 0 && Buffer == NULL) ||
        Payload == NULL)
    {
        return CborErrorInternalError;
    }

    cbor_encoder_init(&encoder, Buffer, *BufferSize, 0);
    CLEANUP_ENCODER_ERR(cbor_encoder_create_array(&encoder, &arrayEncoder, 2));
    CLEANUP_ENCODER_ERR(cbor_encode_uint(&arrayEncoder, Payload->EventId));
    CLEANUP_ENCODER_ERR(cbor_encode_byte_string(
        &arrayEncoder,
        Payload->PayloadSize ? Payload->Payload : Buffer,
        Payload->PayloadSize));
    err = cbor_encoder_close_container_checked(&encoder, &arrayEncoder);
    if (err != CborNoError && err != CborErrorOutOfMemory)
    {
        goto Cleanup;
    }

    *BufferSize = cbor_encoder_get_extra_bytes_needed(&encoder);
    if (*BufferSize)
    {
        err = CborErrorOutOfMemory;
        goto Cleanup;
    }

    *BufferSize = cbor_encoder_get_buffer_size(&encoder, Buffer);

Cleanup:
    return err;
}

#ifndef __GNUC__
#pragma endregion

#pragma region Encode TCPS_LOG_APP_EVENT_RESPONSE
#endif

static
CborError
TcpsCborEncodeAppEventResponse(
    const TCPS_LOG_APP_EVENT_RESPONSE_DATA* const Payload,
    size_t* const BufferSize,
    uint8_t* const Buffer)
/*++

Routine Description:

Parameters:

--*/
{
    CborEncoder encoder, arrayEncoder;
    CborError err = CborNoError;

    if (BufferSize == NULL ||
        (*BufferSize > 0 && Buffer == NULL) ||
        Payload == NULL ||
        Payload->Policies == NULL ||
        Payload->Values == NULL ||
        Payload->Results == NULL)
    {
        return CborErrorInternalError;
    }

    cbor_encoder_init(&encoder, Buffer, *BufferSize, 0);
    CLEANUP_ENCODER_ERR(cbor_encoder_create_array(
        &encoder, 
        &arrayEncoder, 
        3 + 2 * Payload->WrittenCount + Payload->ResultsCount));
    CLEANUP_ENCODER_ERR(cbor_encode_int(&arrayEncoder, Payload->ApprovedCount));
    CLEANUP_ENCODER_ERR(cbor_encode_int(&arrayEncoder, Payload->WrittenCount));
    CLEANUP_ENCODER_ERR(cbor_encode_int(&arrayEncoder, Payload->ResultsCount));
    for (int i = 0; i < Payload->WrittenCount; i++)
    {
        CLEANUP_ENCODER_ERR(cbor_encode_text_string(
            &arrayEncoder,
            Payload->Policies[i],
            strlen(Payload->Policies[i]) + 1));
    }
    for (int i = 0; i < Payload->WrittenCount; i++)
    {
        CLEANUP_ENCODER_ERR(cbor_encode_text_string(
            &arrayEncoder,
            Payload->Values[i],
            strlen(Payload->Values[i]) + 1));
    }
    for (int i = 0; i < Payload->ResultsCount; i++)
    {
        CLEANUP_ENCODER_ERR(cbor_encode_int(
            &arrayEncoder,
            Payload->Results[i]));
    }
    err = cbor_encoder_close_container_checked(&encoder, &arrayEncoder);
    if (err != CborNoError && err != CborErrorOutOfMemory)
    {
        goto Cleanup;
    }

    *BufferSize = cbor_encoder_get_extra_bytes_needed(&encoder);
    if (*BufferSize)
    {
        err = CborErrorOutOfMemory;
        goto Cleanup;
    }

    *BufferSize = cbor_encoder_get_buffer_size(&encoder, Buffer);

Cleanup:
    return err;
}

#ifndef __GNUC__
#pragma endregion

#pragma region Encode TCPS_LOG_APP_EVENT_RESPONSE_FAILED
#endif

static
CborError
TcpsCborEncodeAppEventResponseFailed(
    const TCPS_LOG_APP_EVENT_RESPONSE_FAILED_DATA* const Payload,
    size_t* const BufferSize,
    uint8_t* const Buffer)
/*++

Routine Description:

Parameters:

--*/
{
    CborEncoder encoder, arrayEncoder;
    CborError err = CborNoError;

    if (BufferSize == NULL ||
        (*BufferSize > 0 && Buffer == NULL) ||
        Payload == NULL)
    {
        return CborErrorInternalError;
    }

    cbor_encoder_init(&encoder, Buffer, *BufferSize, 0);
    CLEANUP_ENCODER_ERR(cbor_encoder_create_array(&encoder, &arrayEncoder, 1));
    CLEANUP_ENCODER_ERR(cbor_encode_int(&arrayEncoder, Payload->StatusCode));
    err = cbor_encoder_close_container_checked(&encoder, &arrayEncoder);
    if (err != CborNoError && err != CborErrorOutOfMemory)
    {
        goto Cleanup;
    }

    *BufferSize = cbor_encoder_get_extra_bytes_needed(&encoder);
    if (*BufferSize)
    {
        err = CborErrorOutOfMemory;
        goto Cleanup;
    }

    *BufferSize = cbor_encoder_get_buffer_size(&encoder, Buffer);

Cleanup:
    return err;
}

#ifndef __GNUC__
#pragma endregion

#pragma region Encode TCPS_LOG_APP_EVENT_AUTO_APPROVED
#endif

static
CborError
TcpsCborEncodeAppEventAutoApproved(
    const TCPS_LOG_APP_EVENT_AUTO_APPROVED_DATA* const Payload,
    size_t* const BufferSize,
    uint8_t* const Buffer)
/*++

Routine Description:

Parameters:

--*/
{
    CborEncoder encoder, arrayEncoder;
    CborError err = CborNoError;

    if (BufferSize == NULL ||
        (*BufferSize > 0 && Buffer == NULL) ||
        Payload == NULL ||
        Payload->Policy == NULL ||
        Payload->Value == NULL)
    {
        return CborErrorInternalError;
    }

    cbor_encoder_init(&encoder, Buffer, *BufferSize, 0);
    CLEANUP_ENCODER_ERR(cbor_encoder_create_array(&encoder, &arrayEncoder, 2));
    CLEANUP_ENCODER_ERR(cbor_encode_text_string(
        &arrayEncoder,
        Payload->Policy,
        strlen(Payload->Policy) + 1));
    CLEANUP_ENCODER_ERR(cbor_encode_text_string(
        &arrayEncoder,
        Payload->Value,
        strlen(Payload->Value) + 1));
    err = cbor_encoder_close_container_checked(&encoder, &arrayEncoder);
    if (err != CborNoError && err != CborErrorOutOfMemory)
    {
        goto Cleanup;
    }

    *BufferSize = cbor_encoder_get_extra_bytes_needed(&encoder);
    if (*BufferSize)
    {
        err = CborErrorOutOfMemory;
        goto Cleanup;
    }

    *BufferSize = cbor_encoder_get_buffer_size(&encoder, Buffer);

Cleanup:
    return err;
}

#ifndef __GNUC__
#pragma endregion

#pragma region Encode TCPS_LOG_APP_EVENT_MANUAL_APPROVED
#endif

static
CborError
TcpsCborEncodeAppEventManualApproved(
    const TCPS_LOG_APP_EVENT_MANUAL_APPROVED_DATA* const Payload,
    size_t* const BufferSize,
    uint8_t* const Buffer)
/*++

Routine Description:

Parameters:

--*/
{
    CborEncoder encoder, arrayEncoder;
    CborError err = CborNoError;

    if (BufferSize == NULL ||
        (*BufferSize > 0 && Buffer == NULL) ||
        Payload == NULL ||
        Payload->Policy == NULL ||
        Payload->Value == NULL)
    {
        return CborErrorInternalError;
    }

    cbor_encoder_init(&encoder, Buffer, *BufferSize, 0);
    CLEANUP_ENCODER_ERR(cbor_encoder_create_array(&encoder, &arrayEncoder, 3));
    CLEANUP_ENCODER_ERR(cbor_encode_text_string(
        &arrayEncoder,
        Payload->Policy,
        strlen(Payload->Policy) + 1));
    CLEANUP_ENCODER_ERR(cbor_encode_text_string(
        &arrayEncoder,
        Payload->Value,
        strlen(Payload->Value) + 1));
    CLEANUP_ENCODER_ERR(cbor_encode_int(&arrayEncoder, Payload->FingerprintSlotId));
    err = cbor_encoder_close_container_checked(&encoder, &arrayEncoder);
    if (err != CborNoError && err != CborErrorOutOfMemory)
    {
        goto Cleanup;
    }

    *BufferSize = cbor_encoder_get_extra_bytes_needed(&encoder);
    if (*BufferSize)
    {
        err = CborErrorOutOfMemory;
        goto Cleanup;
    }

    *BufferSize = cbor_encoder_get_buffer_size(&encoder, Buffer);

Cleanup:
    return err;
}

#ifndef __GNUC__
#pragma endregion

#pragma region Encode TCPS_LOG_APP_EVENT_MANUAL_REJECTED
#endif

static
CborError
TcpsCborEncodeAppEventManualRejected(
    const TCPS_LOG_APP_EVENT_MANUAL_REJECTED_DATA* const Payload,
    size_t* const BufferSize,
    uint8_t* const Buffer)
/*++

Routine Description:

Parameters:

--*/
{
    return TcpsCborEncodeAppEventAutoApproved(
        (TCPS_LOG_APP_EVENT_AUTO_APPROVED_DATA*)Payload,
        BufferSize,
        Buffer);
}

#ifndef __GNUC__
#pragma endregion

#pragma endregion

#pragma region Gateway Loggers
#endif

static
Tcps_StatusCode
TcpsLogAppEventInternal(
    const TCPS_LOG_APP_EVENT_DATA* const Payload)
{
    Tcps_StatusCode uStatus = Tcps_Good;
    size_t bufferSize = 0;
    uint8_t* buffer = NULL;
    CborError err = TcpsCborEncodeAppEvent(Payload, &bufferSize, buffer);
    if (err == CborErrorOutOfMemory)
    {
        buffer = TCPS_ALLOC(bufferSize);
        if (buffer != NULL)
        {
            err = TcpsCborEncodeAppEvent(Payload, &bufferSize, buffer);
        }
    }

    if (err != CborNoError)
    {
        uStatus = Tcps_Bad;
        goto Exit;
    }

    uStatus = TcpsLogAppEvent(buffer, bufferSize);

Exit:
    if (buffer != NULL)
    {
        TCPS_FREE(buffer);
    }

    return uStatus;
}

Tcps_StatusCode
TcpsLogAppEventResponse(
    const TCPS_LOG_APP_EVENT_RESPONSE_DATA* const Payload)
{
    TCPS_LOG_APP_EVENT_DATA eventData = { 0 };
    Tcps_StatusCode uStatus = Tcps_Good;
    size_t bufferSize = 0;
    uint8_t* buffer = NULL;
    CborError err = TcpsCborEncodeAppEventResponse(Payload, &bufferSize, buffer);
    if (err == CborErrorOutOfMemory)
    {
        buffer = TCPS_ALLOC(bufferSize);
        if (buffer != NULL)
        {
            err = TcpsCborEncodeAppEventResponse(Payload, &bufferSize, buffer);
        }
    }

    if (err != CborNoError)
    {
        uStatus = Tcps_Bad;
        goto Exit;
    }

    eventData.EventId = TCPS_LOG_APP_EVENT_RESPONSE;
    eventData.Payload = buffer;
    eventData.PayloadSize = bufferSize;

    uStatus = TcpsLogAppEventInternal(&eventData);

Exit:
    if (buffer != NULL)
    {
        TCPS_FREE(buffer);
    }

    return uStatus;
}

Tcps_StatusCode
TcpsLogAppEventResponseFailed(
    const TCPS_LOG_APP_EVENT_RESPONSE_FAILED_DATA* const Payload)
{
    TCPS_LOG_APP_EVENT_DATA eventData = { 0 };
    Tcps_StatusCode uStatus = Tcps_Good;
    size_t bufferSize = 0;
    uint8_t* buffer = NULL;
    CborError err = TcpsCborEncodeAppEventResponseFailed(Payload, &bufferSize, buffer);
    if (err == CborErrorOutOfMemory)
    {
        buffer = TCPS_ALLOC(bufferSize);
        if (buffer != NULL)
        {
            err = TcpsCborEncodeAppEventResponseFailed(Payload, &bufferSize, buffer);
        }
    }

    if (err != CborNoError)
    {
        uStatus = Tcps_Bad;
        goto Exit;
    }

    eventData.EventId = TCPS_LOG_APP_EVENT_RESPONSE_FAILED;
    eventData.Payload = buffer;
    eventData.PayloadSize = bufferSize;

    uStatus = TcpsLogAppEventInternal(&eventData);

Exit:
    if (buffer != NULL)
    {
        TCPS_FREE(buffer);
    }

    return uStatus;
}

Tcps_StatusCode
TcpsLogAppEventAutoApproved(
    const TCPS_LOG_APP_EVENT_AUTO_APPROVED_DATA* const Payload)
{
    TCPS_LOG_APP_EVENT_DATA eventData = { 0 };
    Tcps_StatusCode uStatus = Tcps_Good;
    size_t bufferSize = 0;
    uint8_t* buffer = NULL;
    CborError err = TcpsCborEncodeAppEventAutoApproved(Payload, &bufferSize, buffer);
    if (err == CborErrorOutOfMemory)
    {
        buffer = TCPS_ALLOC(bufferSize);
        if (buffer != NULL)
        {
            err = TcpsCborEncodeAppEventAutoApproved(Payload, &bufferSize, buffer);
        }
    }

    if (err != CborNoError)
    {
        uStatus = Tcps_Bad;
        goto Exit;
    }

    eventData.EventId = TCPS_LOG_APP_EVENT_AUTO_APPROVED;
    eventData.Payload = buffer;
    eventData.PayloadSize = bufferSize;

    uStatus = TcpsLogAppEventInternal(&eventData);

Exit:
    if (buffer != NULL)
    {
        TCPS_FREE(buffer);
    }

    return uStatus;
}

Tcps_StatusCode
TcpsLogAppEventInitialized(void)
{
    Tcps_StatusCode uStatus = Tcps_Good;

    TCPS_LOG_APP_EVENT_DATA eventData = { 0 };
    eventData.EventId = TCPS_LOG_APP_EVENT_INITIALIZED;
    eventData.Payload = NULL;
    eventData.PayloadSize = 0;

    uStatus = TcpsLogAppEventInternal(&eventData);

    return uStatus;
}

Tcps_StatusCode
TcpsLogAppEventManualApproved(
    const TCPS_LOG_APP_EVENT_MANUAL_APPROVED_DATA* const Payload)
{
    TCPS_LOG_APP_EVENT_DATA eventData = { 0 };
    Tcps_StatusCode uStatus = Tcps_Good;
    size_t bufferSize = 0;
    uint8_t* buffer = NULL;
    CborError err = TcpsCborEncodeAppEventManualApproved(Payload, &bufferSize, buffer);
    if (err == CborErrorOutOfMemory)
    {
        buffer = TCPS_ALLOC(bufferSize);
        if (buffer != NULL)
        {
            err = TcpsCborEncodeAppEventManualApproved(Payload, &bufferSize, buffer);
        }
    }

    if (err != CborNoError)
    {
        uStatus = Tcps_Bad;
        goto Exit;
    }

    eventData.EventId = TCPS_LOG_APP_EVENT_MANUAL_APPROVED;
    eventData.Payload = buffer;
    eventData.PayloadSize = bufferSize;

    uStatus = TcpsLogAppEventInternal(&eventData);

Exit:
    if (buffer != NULL)
    {
        TCPS_FREE(buffer);
    }

    return uStatus;
}

Tcps_StatusCode
TcpsLogAppEventManualRejected(
    const TCPS_LOG_APP_EVENT_MANUAL_REJECTED_DATA* const Payload)
{
    TCPS_LOG_APP_EVENT_DATA eventData = { 0 };
    Tcps_StatusCode uStatus = Tcps_Good;
    size_t bufferSize = 0;
    uint8_t* buffer = NULL;
    CborError err = TcpsCborEncodeAppEventManualRejected(Payload, &bufferSize, buffer);
    if (err == CborErrorOutOfMemory)
    {
        buffer = TCPS_ALLOC(bufferSize);
        if (buffer != NULL)
        {
            err = TcpsCborEncodeAppEventManualRejected(Payload, &bufferSize, buffer);
        }
    }

    if (err != CborNoError)
    {
        uStatus = Tcps_Bad;
        goto Exit;
    }

    eventData.EventId = TCPS_LOG_APP_EVENT_MANUAL_REJECTED;
    eventData.Payload = buffer;
    eventData.PayloadSize = bufferSize;

    uStatus = TcpsLogAppEventInternal(&eventData);

Exit:
    if (buffer != NULL)
    {
        TCPS_FREE(buffer);
    }

    return uStatus;
}

#ifndef __GNUC__
#pragma endregion
#endif
