/* Copyright (c) Microsoft Corporation.  All Rights Reserved. */
/* Licensed under the MIT License. */
#include <stdlib.h>
#include <time.h>
#include <openenclave/enclave.h>
#include "tcps_stdio_t.h"
#include <cbor.h>

#include "tcpsTypes.h"
#include "TcpsLogv2.h"

#include "ICertUtil.h"

// TODO: add SAL to implementation functions

#pragma region Attributes

typedef struct TCPS_LOG_LOCAL_TRANSPORT
{
    PTCPS_LOG_LOCAL_WRITE WriteLocalEventHandler;
    PTCPS_LOG_LOCAL_READ ReadLocalBlockHandler;
    PTCPS_LOG_LOCAL_CLEAR ClearLocalBlockHandler;
    void *HandlerContext;
} TCPS_LOG_LOCAL_TRANSPORT;

typedef struct TCPS_LOG_REMOTE_TRANSPORT
{
    PTCPS_LOG_REMOTE_WRITE WriteRemoteBlockHandler;
    void *HandlerContext;
} TCPS_LOG_REMOTE_TRANSPORT;

struct TCPS_LOG_ATTRIBUTES
{
    const TCPS_IDENTITY_PRIVATE *SigningIdentity;
    const TCPS_IDENTITY_PUBLIC *ValidationIdentity;

    TCPS_LOG_CATEGORY *Categories;
    size_t CategoryCount;

    TCPS_LOG_LOCAL_TRANSPORT *LocalTransport;
    TCPS_LOG_REMOTE_TRANSPORT *RemoteTransport;

    PTCPS_LOG_TIME GetTimeHandler;
};

#pragma endregion Types

#pragma region Encoding

static CborError
TcpsCborDecodeEnd(
    CborValue *ItemIter,
    CborValue *ArrayIter)
{
    CborError err = CborNoError;

    if (ItemIter == NULL ||
        ArrayIter == NULL)
    {
        return CborErrorInternalError;
    }

    // verify that no more items are left in the array
    if (!cbor_value_at_end(ArrayIter))
    {
        err = CborErrorTooManyItems;
        goto Cleanup;
    }

    // verify that no more items are left past the array
    CLEANUP_DECODER_ERR(cbor_value_leave_container(ItemIter, ArrayIter));
    if (!cbor_value_at_end(ItemIter))
    {
        err = CborErrorTooManyItems;
        goto Cleanup;
    }

Cleanup:
    return err;
}

#pragma endregion Encoding

#pragma region Signing

typedef struct TCPS_LOG_SIGNED_PAYLOAD
{
    const uint8_t *Payload;
    size_t PayloadSize;
    TCPS_IDENTITY_SIGNATURE Signature;
    TCPS_IDENTITY_PUBLIC_SERIALIZED SerializedValidationIdentity;
} TCPS_LOG_SIGNED_PAYLOAD;

static CborError
TcpsEncodeSignedLogPayload(
    const TCPS_LOG_SIGNED_PAYLOAD *SignedPayload,
    uint8_t *EncodedSignedPayload,
    size_t *EncodedSignedPayloadSize)
{
    CborEncoder encoder, arrayEncoder;
    CborError err = CborNoError;

    if (SignedPayload == NULL ||
        EncodedSignedPayloadSize == NULL)
    {
        return CborErrorInternalError;
    }

    // create signed log payload container
    cbor_encoder_init(&encoder, EncodedSignedPayload, *EncodedSignedPayloadSize, 0);
    CLEANUP_ENCODER_ERR(cbor_encoder_create_array(&encoder, &arrayEncoder, 3));

    // encode signed log payload
    CLEANUP_ENCODER_ERR(cbor_encode_byte_string(&arrayEncoder, SignedPayload->Payload, SignedPayload->PayloadSize));
    CLEANUP_ENCODER_ERR(cbor_encode_byte_string(&arrayEncoder, SignedPayload->SerializedValidationIdentity, sizeof(SignedPayload->SerializedValidationIdentity)));
    CLEANUP_ENCODER_ERR(cbor_encode_byte_string(&arrayEncoder, &SignedPayload->Signature, sizeof(SignedPayload->Signature)));

    // close signed log payload container
    err = cbor_encoder_close_container_checked(&encoder, &arrayEncoder);
    if (err != CborNoError)
    {
        goto Cleanup;
    }

    // verify no additional memory is required
    *EncodedSignedPayloadSize = cbor_encoder_get_extra_bytes_needed(&encoder);
    if (*EncodedSignedPayloadSize)
    {
        err = CborErrorOutOfMemory;
        goto Cleanup;
    }
    *EncodedSignedPayloadSize = cbor_encoder_get_buffer_size(&encoder, EncodedSignedPayload);

Cleanup:
    return err;
}

static CborError
TcpsDecodeSignedLogPayload(
    TCPS_LOG_SIGNED_PAYLOAD *SignedPayload,
    const uint8_t *EncodedSignedPayload,
    size_t EncodedSignedPayloadSize)
{
    CborParser parser;
    CborValue itemIter, arrayIter;
    CborError err = CborNoError;
    uint8_t *buffer;
    size_t bufferSize;

    if (SignedPayload == NULL ||
        EncodedSignedPayload == NULL)
    {
        return CborErrorInternalError;
    }

    // open the encoded container
    CLEANUP_DECODER_ERR(cbor_parser_init(EncodedSignedPayload, EncodedSignedPayloadSize, 0, &parser, &itemIter));
    if (!cbor_value_is_array(&itemIter))
    {
        err = CborErrorIllegalType;
        goto Cleanup;
    }
    CLEANUP_DECODER_ERR(cbor_value_enter_container(&itemIter, &arrayIter));

    // extract the signed payload
    if (!cbor_value_is_byte_string(&arrayIter))
    {
        err = CborErrorIllegalType;
        goto Cleanup;
    }
    CLEANUP_DECODER_ERR(cbor_value_ref_byte_string(&arrayIter, &SignedPayload->Payload, &SignedPayload->PayloadSize, &arrayIter));

    // extract the serialized validation identity
    if (!cbor_value_is_byte_string(&arrayIter))
    {
        err = CborErrorIllegalType;
        goto Cleanup;
    }
    CLEANUP_DECODER_ERR(cbor_value_ref_byte_string(&arrayIter, &buffer, &bufferSize, &arrayIter));
    if (bufferSize != sizeof(TCPS_IDENTITY_PUBLIC_SERIALIZED))
    {
        err = CborErrorIllegalType;
        goto Cleanup;
    }
    memcpy(SignedPayload->SerializedValidationIdentity, buffer, bufferSize);

    // extract the signed payload digest
    if (!cbor_value_is_byte_string(&arrayIter))
    {
        err = CborErrorIllegalType;
        goto Cleanup;
    }
    CLEANUP_DECODER_ERR(cbor_value_ref_byte_string(&arrayIter, &buffer, &bufferSize, &arrayIter));
    if (bufferSize != sizeof(TCPS_IDENTITY_SIGNATURE))
    {
        err = CborErrorIllegalType;
        goto Cleanup;
    }
    memcpy(&SignedPayload->Signature, buffer, bufferSize);

    // close the encoded container
    CLEANUP_DECODER_ERR(TcpsCborDecodeEnd(&itemIter, &arrayIter));

Cleanup:
    return err;
}

static oe_result_t
TcpsSignLogPayload(
    TCPS_LOG_SIGNED_PAYLOAD *SignedPayload,
    const uint8_t *Payload,
    size_t PayloadSize,
    const TCPS_IDENTITY_PUBLIC *ValidationIdentity,
    const TCPS_IDENTITY_PRIVATE *SigningIdentity)
{
    oe_result_t status = OE_OK;
    TCPS_SHA256_DIGEST digest;
    RIOT_STATUS signStatus;

    if (SignedPayload == NULL ||
        Payload == NULL ||
        ValidationIdentity == NULL ||
        SigningIdentity == NULL)
    {
        status = OE_INVALID_PARAMETER;
        goto Exit;
    }

    // retrieve the payload digest
    TcpsSHA256Block(Payload, PayloadSize, digest);

    // sign the payload digest
    signStatus = RIOT_DSASignDigest(
        digest,
        SigningIdentity,
        &SignedPayload->Signature);
    if (signStatus != RIOT_SUCCESS)
    {
        status = OE_FAILURE;
        goto Exit;
    }

    // export the validation identity
    RiotCrypt_ExportEccPub(ValidationIdentity, SignedPayload->SerializedValidationIdentity, NULL);

    // output the signed data
    SignedPayload->Payload = Payload;
    SignedPayload->PayloadSize = PayloadSize;

Exit:
    return status;
}

static oe_result_t
TcpsValidateSignedLogPayload(
    const TCPS_LOG_SIGNED_PAYLOAD *SignedPayload)
{
    oe_result_t status = OE_OK;
    TCPS_SHA256_DIGEST digest;
    TCPS_IDENTITY_PUBLIC validationIdentity;
    RIOT_STATUS signStatus;

    if (SignedPayload == NULL)
    {
        status = OE_INVALID_PARAMETER;
        goto Exit;
    }

    // import the validation identity from its serialized form
    if (RiotCrypt_ImportEccPub(
            SignedPayload->SerializedValidationIdentity,
            sizeof(SignedPayload->SerializedValidationIdentity),
            &validationIdentity) != RIOT_SUCCESS)
    {
        status = OE_FAILURE;
        goto Exit;
    }

    // retrieve the payload digest
    TcpsSHA256Block(SignedPayload->Payload, SignedPayload->PayloadSize, digest);

    // verify that the signature is valid
    signStatus = RIOT_DSAVerifyDigest(
        digest,
        &SignedPayload->Signature,
        &validationIdentity);
    if (signStatus != RIOT_SUCCESS)
    {
        status = OE_FAILURE;
        goto Exit;
    }

Exit:
    return status;
}

#pragma endregion Signing

#pragma region Versioning

typedef enum TCPS_LOG_VERSION
{
    TCPS_LOG_VERSION_1
} TCPS_LOG_VERSION;

typedef struct TCPS_LOG_VERSIONED_PAYLOAD
{
    const uint8_t *Payload;
    size_t PayloadSize;
    TCPS_LOG_VERSION Version;
} TCPS_LOG_VERSIONED_PAYLOAD;

static CborError
TcpsEncodeVersionedPayload(
    const uint8_t *Payload,
    size_t PayloadSize,
    uint8_t *EncodedVersionedPayload,
    size_t *EncodedVersionedPayloadSize)
{
    CborEncoder encoder, arrayEncoder;
    CborError err = CborNoError;

    if (Payload == NULL ||
        EncodedVersionedPayloadSize == NULL)
    {
        return CborErrorInternalError;
    }

    // create versioned log payload container
    cbor_encoder_init(&encoder, EncodedVersionedPayload, *EncodedVersionedPayloadSize, 0);
    CLEANUP_ENCODER_ERR(cbor_encoder_create_array(&encoder, &arrayEncoder, 2));

    // encode versioned log payload
    CLEANUP_ENCODER_ERR(cbor_encode_uint(&arrayEncoder, TCPS_LOG_VERSION_1));
    CLEANUP_ENCODER_ERR(cbor_encode_byte_string(&arrayEncoder, Payload, PayloadSize));

    // close versioned log payload container
    err = cbor_encoder_close_container_checked(&encoder, &arrayEncoder);
    if (err != CborNoError)
    {
        goto Cleanup;
    }

    // verify no additional memory is required
    *EncodedVersionedPayloadSize = cbor_encoder_get_extra_bytes_needed(&encoder);
    if (*EncodedVersionedPayloadSize)
    {
        err = CborErrorOutOfMemory;
        goto Cleanup;
    }
    *EncodedVersionedPayloadSize = cbor_encoder_get_buffer_size(&encoder, EncodedVersionedPayload);

Cleanup:
    return err;
}

static CborError
TcpsDecodeVersionedPayload(
    TCPS_LOG_VERSIONED_PAYLOAD *VersionedPayload,
    const uint8_t *EncodedVersionedPayload,
    size_t EncodedVersionedPayloadSize)
{
    CborParser parser;
    CborValue itemIter, arrayIter;
    CborError err = CborNoError;
    uint64_t version64;

    if (VersionedPayload == NULL ||
        EncodedVersionedPayload == NULL)
    {
        return CborErrorInternalError;
    }

    // open the encoded container
    CLEANUP_DECODER_ERR(cbor_parser_init(EncodedVersionedPayload, EncodedVersionedPayloadSize, 0, &parser, &itemIter));
    if (!cbor_value_is_array(&itemIter))
    {
        err = CborErrorIllegalType;
        goto Cleanup;
    }
    CLEANUP_DECODER_ERR(cbor_value_enter_container(&itemIter, &arrayIter));

    // extract the version
    if (!cbor_value_is_unsigned_integer(&arrayIter))
    {
        err = CborErrorIllegalType;
        goto Cleanup;
    }
    CLEANUP_DECODER_ERR(cbor_value_get_uint64(&arrayIter, &version64));
    if (version64 > UINT32_MAX)
    {
        err = CborErrorUnknownType;
        goto Cleanup;
    }
    VersionedPayload->Version = (TCPS_LOG_VERSION)version64;
    CLEANUP_DECODER_ERR(cbor_value_advance_fixed(&arrayIter));

    // extract the versioned payload
    if (!cbor_value_is_byte_string(&arrayIter))
    {
        err = CborErrorIllegalType;
        goto Cleanup;
    }
    CLEANUP_DECODER_ERR(cbor_value_ref_byte_string(&arrayIter, &VersionedPayload->Payload, &VersionedPayload->PayloadSize, &arrayIter));

    // close the encoded container
    CLEANUP_DECODER_ERR(TcpsCborDecodeEnd(&itemIter, &arrayIter));

Cleanup:
    return err;
}

#pragma endregion Versioning

#pragma region Tagging

typedef enum TCPS_LOG_ENCODED_PAYLOAD_TYPE
{
    TCPS_LOG_ENCODED_PAYLOAD_TYPE_EVENT,
    TCPS_LOG_ENCODED_PAYLOAD_TYPE_BLOCK,
    TCPS_LOG_ENCODED_PAYLOAD_TYPE_SIGNED,
    TCPS_LOG_ENCODED_PAYLOAD_TYPE_VERSIONED
} TCPS_LOG_ENCODED_PAYLOAD_TYPE;

typedef struct TCPS_LOG_TAGGED_PAYLOAD
{
    const uint8_t *Payload;
    size_t PayloadSize;
    TCPS_LOG_ENCODED_PAYLOAD_TYPE Tag;
} TCPS_LOG_TAGGED_PAYLOAD;

static CborError
TcpsEncodeTaggedPayload(
    const uint8_t *Payload,
    size_t PayloadSize,
    TCPS_LOG_ENCODED_PAYLOAD_TYPE Tag,
    uint8_t *EncodedTaggedPayload,
    size_t *EncodedTaggedPayloadSize)
{
    CborEncoder encoder, arrayEncoder;
    CborError err = CborNoError;

    if (Payload == NULL ||
        EncodedTaggedPayloadSize == NULL)
    {
        return CborErrorInternalError;
    }

    // create versioned log payload container
    cbor_encoder_init(&encoder, EncodedTaggedPayload, *EncodedTaggedPayloadSize, 0);
    CLEANUP_ENCODER_ERR(cbor_encoder_create_array(&encoder, &arrayEncoder, 2));

    // encode tagged payload
    CLEANUP_ENCODER_ERR(cbor_encode_uint(&arrayEncoder, Tag));
    CLEANUP_ENCODER_ERR(cbor_encode_byte_string(&arrayEncoder, Payload, PayloadSize));

    // close versioned log payload container
    err = cbor_encoder_close_container_checked(&encoder, &arrayEncoder);
    if (err != CborNoError)
    {
        goto Cleanup;
    }

    // verify no additional memory is required
    *EncodedTaggedPayloadSize = cbor_encoder_get_extra_bytes_needed(&encoder);
    if (*EncodedTaggedPayloadSize)
    {
        err = CborErrorOutOfMemory;
        goto Cleanup;
    }
    *EncodedTaggedPayloadSize = cbor_encoder_get_buffer_size(&encoder, EncodedTaggedPayload);

Cleanup:
    return err;
}

static CborError
TcpsDecodeTaggedPayload(
    TCPS_LOG_TAGGED_PAYLOAD *TaggedPayload,
    const uint8_t *EncodedTaggedPayload,
    size_t EncodedTaggedPayloadSize)
{
    CborParser parser;
    CborValue itemIter, arrayIter;
    CborError err = CborNoError;
    uint64_t tag64;

    if (TaggedPayload == NULL ||
        EncodedTaggedPayload == NULL)
    {
        return CborErrorInternalError;
    }

    // open the encoded container
    CLEANUP_DECODER_ERR(cbor_parser_init(EncodedTaggedPayload, EncodedTaggedPayloadSize, 0, &parser, &itemIter));
    if (!cbor_value_is_array(&itemIter))
    {
        err = CborErrorIllegalType;
        goto Cleanup;
    }
    CLEANUP_DECODER_ERR(cbor_value_enter_container(&itemIter, &arrayIter));

    // extract the tag
    if (!cbor_value_is_unsigned_integer(&arrayIter))
    {
        err = CborErrorIllegalType;
        goto Cleanup;
    }
    CLEANUP_DECODER_ERR(cbor_value_get_uint64(&arrayIter, &tag64));
    if (tag64 > UINT32_MAX)
    {
        err = CborErrorUnknownType;
        goto Cleanup;
    }
    TaggedPayload->Tag = (TCPS_LOG_VERSION)tag64;
    CLEANUP_DECODER_ERR(cbor_value_advance_fixed(&arrayIter));

    // extract the tagged payload
    if (!cbor_value_is_byte_string(&arrayIter))
    {
        err = CborErrorIllegalType;
        goto Cleanup;
    }
    CLEANUP_DECODER_ERR(cbor_value_ref_byte_string(&arrayIter, &TaggedPayload->Payload, &TaggedPayload->PayloadSize, &arrayIter));

    // close the encoded container
    CLEANUP_DECODER_ERR(TcpsCborDecodeEnd(&itemIter, &arrayIter));

Cleanup:
    return err;
}

#pragma endregion Tagging

#pragma region Events

typedef struct TCPS_LOG_EVENT
{
    const uint8_t *Payload;
    size_t PayloadSize;
    __time64_t Timestamp;
    TCPS_SHA256_DIGEST LogChainDigest;
} TCPS_LOG_EVENT;

static CborError
TcpsEncodeLogEvent(
    const TCPS_LOG_EVENT *Event,
    uint8_t *EncodedEvent,
    size_t *EncodedEventSize)
{
    CborError err = CborNoError;
    CborEncoder encoder, arrayEncoder;

    if (Event == NULL ||
        EncodedEventSize == NULL)
    {
        return CborErrorInternalError;
    }

    // create event payload container
    cbor_encoder_init(&encoder, EncodedEvent, *EncodedEventSize, 0);
    CLEANUP_ENCODER_ERR(cbor_encoder_create_array(&encoder, &arrayEncoder, 3));

    // encode event payload
    CLEANUP_ENCODER_ERR(cbor_encode_byte_string(&arrayEncoder, Event->LogChainDigest, sizeof(TCPS_SHA256_DIGEST)));
    CLEANUP_ENCODER_ERR(cbor_encode_int(&arrayEncoder, Event->Timestamp));
    CLEANUP_ENCODER_ERR(cbor_encode_byte_string(&arrayEncoder, Event->Payload, Event->PayloadSize));

    // close event payload container
    err = cbor_encoder_close_container_checked(&encoder, &arrayEncoder);
    if (err != CborNoError)
    {
        goto Cleanup;
    }

    // verify no additional memory is required
    *EncodedEventSize = cbor_encoder_get_extra_bytes_needed(&encoder);
    if (*EncodedEventSize)
    {
        err = CborErrorOutOfMemory;
        goto Cleanup;
    }
    *EncodedEventSize = cbor_encoder_get_buffer_size(&encoder, EncodedEvent);

Cleanup:
    return err;
}

static CborError
TcpsDecodeLogEvent(
    TCPS_LOG_EVENT *Event,
    const uint8_t *EncodedEvent,
    size_t EncodedEventSize)
{
    CborParser parser;
    CborValue itemIter, arrayIter;
    CborError err = CborNoError;
    uint8_t *buffer;
    size_t bufferSize;

    if (Event == NULL ||
        EncodedEvent == NULL)
    {
        return CborErrorInternalError;
    }

    // open the encoded container
    CLEANUP_DECODER_ERR(cbor_parser_init(EncodedEvent, EncodedEventSize, 0, &parser, &itemIter));
    if (!cbor_value_is_array(&itemIter))
    {
        err = CborErrorIllegalType;
        goto Cleanup;
    }
    CLEANUP_DECODER_ERR(cbor_value_enter_container(&itemIter, &arrayIter));

    // extract the log chain digest
    if (!cbor_value_is_byte_string(&arrayIter))
    {
        err = CborErrorIllegalType;
        goto Cleanup;
    }
    CLEANUP_DECODER_ERR(cbor_value_ref_byte_string(&arrayIter, &buffer, &bufferSize, &arrayIter));
    if (bufferSize != sizeof(TCPS_SHA256_DIGEST))
    {
        err = CborErrorIllegalType;
        goto Cleanup;
    }
    memcpy(Event->LogChainDigest, buffer, bufferSize);

    // extract the timestamp
    if (!cbor_value_is_integer(&arrayIter))
    {
        err = CborErrorIllegalType;
        goto Cleanup;
    }
    CLEANUP_DECODER_ERR(cbor_value_get_int64_checked(&arrayIter, &Event->Timestamp));
    CLEANUP_DECODER_ERR(cbor_value_advance_fixed(&arrayIter));

    // extract the payload
    if (!cbor_value_is_byte_string(&arrayIter))
    {
        err = CborErrorIllegalType;
        goto Cleanup;
    }
    CLEANUP_DECODER_ERR(cbor_value_ref_byte_string(&arrayIter, &Event->Payload, &Event->PayloadSize, &arrayIter));

    // close the encoded container
    CLEANUP_DECODER_ERR(TcpsCborDecodeEnd(&itemIter, &arrayIter));

Cleanup:
    return err;
}

#pragma endregion Events

#pragma region Categories

typedef struct TCPS_LOG_CATEGORY
{
    const char *Label;
    TCPS_SHA256_DIGEST InitialDigest;
    TCPS_SHA256_DIGEST CurrentDigest;
    const uint8_t *CounterId;
    size_t CounterIdSize;

    PTCPS_LOG_CATEGORY_PERSIST PersistCategoryHandler;
    PTCPS_LOG_CATEGORY_RECOVER RecoverCategoryHandler;
    PTCPS_LOG_COUNTER_CREATE CreateCounterHandler;
    PTCPS_LOG_COUNTER_VALIDATE ValidateCounterHandler;
    PTCPS_LOG_COUNTER_INCREMENTGET IncrementGetCounterHandler;
    void *HandlerContext;
} TCPS_LOG_CATEGORY;

static CborError
TcpsEncodeCategoryWithCounter(
    const TCPS_LOG_CATEGORY *Category,
    const uint8_t *CounterValue,
    size_t CounterValueSize,
    uint8_t *EncodedCategory,
    size_t *EncodedCategorySize)
{
    CborEncoder encoder, arrayEncoder;
    CborError err = CborNoError;

    // need a dummy value for when buffer size is 0 because NULL is an unallowed argument value
    // okay to leave uninitialized - content will never be read
    uint8_t dummy;

    if (Category == NULL ||
        EncodedCategorySize == NULL)
    {
        return CborErrorInternalError;
    }

    // create identity container
    cbor_encoder_init(&encoder, EncodedCategory, *EncodedCategorySize, 0);
    CLEANUP_ENCODER_ERR(cbor_encoder_create_array(&encoder, &arrayEncoder, 4));

    // encode identity fields sans label
    CLEANUP_ENCODER_ERR(cbor_encode_byte_string(&arrayEncoder, Category->CounterIdSize ? Category->CounterId : &dummy, Category->CounterIdSize));
    CLEANUP_ENCODER_ERR(cbor_encode_byte_string(&arrayEncoder, CounterValueSize ? CounterValue : &dummy, CounterValueSize));
    CLEANUP_ENCODER_ERR(cbor_encode_byte_string(&arrayEncoder, Category->InitialDigest, sizeof(TCPS_SHA256_DIGEST)));
    CLEANUP_ENCODER_ERR(cbor_encode_byte_string(&arrayEncoder, Category->CurrentDigest, sizeof(TCPS_SHA256_DIGEST)));

    // close identity container
    err = cbor_encoder_close_container_checked(&encoder, &arrayEncoder);
    if (err != CborNoError)
    {
        goto Cleanup;
    }

    // verify no additional memory is required
    *EncodedCategorySize = cbor_encoder_get_extra_bytes_needed(&encoder);
    if (*EncodedCategorySize)
    {
        err = CborErrorOutOfMemory;
        goto Cleanup;
    }
    *EncodedCategorySize = cbor_encoder_get_buffer_size(&encoder, EncodedCategory);

Cleanup:
    return err;
}

static CborError
TcpsDecodeCategoryWithCounter(
    TCPS_LOG_CATEGORY *Category,
    uint8_t **CounterValue,
    size_t *CounterValueSize,
    const uint8_t *EncodedCategory,
    size_t EncodedCategorySize)
{
    CborParser parser;
    CborValue itemIter, arrayIter;
    CborError err = CborNoError;
    uint8_t *buffer;
    size_t bufferSize;

    if (Category == NULL ||
        CounterValue == NULL ||
        CounterValueSize == NULL ||
        EncodedCategory == NULL)
    {
        return CborErrorInternalError;
    }

    // open the encoded container
    CLEANUP_DECODER_ERR(cbor_parser_init(EncodedCategory, EncodedCategorySize, 0, &parser, &itemIter));
    if (!cbor_value_is_array(&itemIter))
    {
        err = CborErrorIllegalType;
        goto Cleanup;
    }
    CLEANUP_DECODER_ERR(cbor_value_enter_container(&itemIter, &arrayIter));

    // extract the counter id
    if (!cbor_value_is_byte_string(&arrayIter))
    {
        err = CborErrorIllegalType;
        goto Cleanup;
    }
    CLEANUP_DECODER_ERR(cbor_value_ref_byte_string(&arrayIter, &Category->CounterId, &Category->CounterIdSize, &arrayIter));

    // extract the counter value
    if (!cbor_value_is_byte_string(&arrayIter))
    {
        err = CborErrorIllegalType;
        goto Cleanup;
    }
    CLEANUP_DECODER_ERR(cbor_value_ref_byte_string(&arrayIter, CounterValue, CounterValueSize, &arrayIter));

    // extract the initial chain hash
    if (!cbor_value_is_byte_string(&arrayIter))
    {
        err = CborErrorIllegalType;
        goto Cleanup;
    }
    CLEANUP_DECODER_ERR(cbor_value_ref_byte_string(&arrayIter, &buffer, &bufferSize, &arrayIter));
    if (bufferSize != sizeof(TCPS_SHA256_DIGEST))
    {
        err = CborErrorIllegalType;
        goto Cleanup;
    }
    memcpy(Category->InitialDigest, buffer, bufferSize);

    // extract the current chain hash
    if (!cbor_value_is_byte_string(&arrayIter))
    {
        err = CborErrorIllegalType;
        goto Cleanup;
    }
    CLEANUP_DECODER_ERR(cbor_value_ref_byte_string(&arrayIter, &buffer, &bufferSize, &arrayIter));
    if (bufferSize != sizeof(TCPS_SHA256_DIGEST))
    {
        err = CborErrorIllegalType;
        goto Cleanup;
    }
    memcpy(Category->CurrentDigest, buffer, bufferSize);

    // close the encoded container
    CLEANUP_DECODER_ERR(TcpsCborDecodeEnd(&itemIter, &arrayIter));

Cleanup:
    return err;
}

static oe_result_t
TcpsPersistCategoryWithCounter(
    TCPS_LOG_CATEGORY *Category,
    const uint8_t *CounterValue,
    size_t CounterValueSize,
    const TCPS_IDENTITY_PUBLIC *ValidationIdentity,
    const TCPS_IDENTITY_PRIVATE *SigningIdentity)
{
    oe_result_t status = OE_OK;
    CborError err;
    const uint8_t *selectedCounterValue;
    uint8_t *retrievedCounterValue = NULL, *encodedCategory = NULL, *encodedSignedPayload = NULL;
    size_t selectedCounterValueSize = 0, retrievedCounterValueSize = 0, encodedCategorySize = 0, encodedSignedPayloadSize = 0;
    TCPS_LOG_SIGNED_PAYLOAD signedEncodedCategory;

    if (Category == NULL ||
        ValidationIdentity == NULL ||
        SigningIdentity == NULL)
    {
        status = OE_INVALID_PARAMETER;
        goto Exit;
    }

    if (CounterValue == NULL)
    {
        Category->IncrementGetCounterHandler(
            Category->HandlerContext,
            Category->CounterId,
            Category->CounterIdSize,
            &retrievedCounterValue,
            &retrievedCounterValueSize);
        selectedCounterValue = retrievedCounterValue;
        selectedCounterValueSize = retrievedCounterValueSize;
    }
    else
    {
        selectedCounterValue = CounterValue;
        selectedCounterValueSize = CounterValueSize;
    }

    err = TcpsEncodeCategoryWithCounter(
        Category,
        selectedCounterValue,
        selectedCounterValueSize,
        encodedCategory,
        &encodedCategorySize);
    if (err == CborErrorOutOfMemory)
    {
        encodedCategory = malloc(encodedCategorySize);
        if (encodedCategory == NULL)
        {
            status = OE_OUT_OF_MEMORY;
            goto Exit;
        }
        err = TcpsEncodeCategoryWithCounter(
            Category,
            selectedCounterValue,
            selectedCounterValueSize,
            encodedCategory,
            &encodedCategorySize);
    }
    if (err != CborNoError)
    {
        status = OE_FAILURE;
        goto Exit;
    }

    // sign the encoded identity
    status = TcpsSignLogPayload(
        &signedEncodedCategory,
        encodedCategory,
        encodedCategorySize,
        ValidationIdentity,
        SigningIdentity);
    if (status != OE_OK)
    {
        goto Exit;
    }

    // encode the signed identity
    // note: the first attempt should fail, but will tell the amount of memory required
    //       to store the encoding so that we can perform the allocation
    err = TcpsEncodeSignedLogPayload(
        &signedEncodedCategory,
        encodedSignedPayload,
        &encodedSignedPayloadSize);
    if (err == CborErrorOutOfMemory)
    {
        encodedSignedPayload = malloc(encodedSignedPayloadSize);
        if (encodedSignedPayload == NULL)
        {
            status = OE_OUT_OF_MEMORY;
            goto Exit;
        }
        err = TcpsEncodeSignedLogPayload(
            &signedEncodedCategory,
            encodedSignedPayload,
            &encodedSignedPayloadSize);
    }
    if (err != CborNoError)
    {
        status = OE_FAILURE;
        goto Exit;
    }

    // defer to user-provided handler to move payload to persistent storage
    status = Category->PersistCategoryHandler(
        Category->HandlerContext,
        Category->Label,
        encodedSignedPayload,
        encodedSignedPayloadSize);
    if (status != OE_OK)
    {
        goto Exit;
    }

Exit:
    if (retrievedCounterValue != NULL)
    {
        free(retrievedCounterValue);
    }
    if (encodedCategory != NULL)
    {
        free(encodedCategory);
    }
    if (encodedSignedPayload != NULL)
    {
        free(encodedSignedPayload);
    }

    return OE_OK;
}

static oe_result_t
TcpsGetCategory(
    TCPS_LOG_ATTRIBUTES *LogAttributes,
    const char *Label,
    TCPS_LOG_CATEGORY **Category)
{
    *Category = NULL;

    for (size_t i = 0; i < LogAttributes->CategoryCount; i++)
    {
        if (strcmp(Label, LogAttributes->Categories[i].Label) == 0)
        {
            *Category = &LogAttributes->Categories[i];
            break;
        }
    }

    return OE_OK;
}

#pragma endregion Category

#pragma region Blocks

typedef enum TCPS_LOG_VALIDATION_STATE
{
    TCPS_LOG_VALIDATION_STATE_OK,
    TCPS_LOG_VALIDATION_STATE_BREAK,
    TCPS_LOG_VALIDATION_STATE_BAD
} TCPS_LOG_VALIDATION_STATE;

typedef struct TCPS_LOG_BLOCK
{
    const uint8_t *Payload;
    size_t PayloadSize;
    TCPS_SHA256_DIGEST InitialDigest;
    TCPS_SHA256_DIGEST CurrentDigest;
    const char *CategoryLabel;
    TCPS_LOG_VALIDATION_STATE ValidationState;
} TCPS_LOG_BLOCK;

static CborError
TcpsEncodeBlock(
    const TCPS_LOG_CATEGORY *Category,
    TCPS_LOG_VALIDATION_STATE ValidationState,
    const uint8_t *EncodedEvents,
    size_t EncodedEventsSize,
    uint8_t *EncodedBlock,
    size_t *EncodedBlockSize)
{
    CborError err = CborNoError;
    CborEncoder encoder, arrayEncoder;

    if (Category == NULL ||
        EncodedEvents == NULL ||
        EncodedBlockSize == NULL)
    {
        return CborErrorInternalError;
    }

    // create event payload container
    cbor_encoder_init(&encoder, EncodedBlock, *EncodedBlockSize, 0);
    CLEANUP_ENCODER_ERR(cbor_encoder_create_array(&encoder, &arrayEncoder, 5));

    // encode event payload
    CLEANUP_ENCODER_ERR(cbor_encode_byte_string(&arrayEncoder, Category->InitialDigest, sizeof(TCPS_SHA256_DIGEST)));
    CLEANUP_ENCODER_ERR(cbor_encode_byte_string(&arrayEncoder, Category->CurrentDigest, sizeof(TCPS_SHA256_DIGEST)));
    CLEANUP_ENCODER_ERR(cbor_encode_byte_string(&arrayEncoder, (const uint8_t *)Category->Label, strlen(Category->Label) + 1));
    CLEANUP_ENCODER_ERR(cbor_encode_uint(&arrayEncoder, ValidationState));
    CLEANUP_ENCODER_ERR(cbor_encode_byte_string(&arrayEncoder, EncodedEvents, EncodedEventsSize));

    // close event payload container
    err = cbor_encoder_close_container_checked(&encoder, &arrayEncoder);
    if (err != CborNoError)
    {
        goto Cleanup;
    }

    // verify no additional memory is required
    *EncodedBlockSize = cbor_encoder_get_extra_bytes_needed(&encoder);
    if (*EncodedBlockSize)
    {
        err = CborErrorOutOfMemory;
        goto Cleanup;
    }
    *EncodedBlockSize = cbor_encoder_get_buffer_size(&encoder, EncodedBlock);

Cleanup:
    return err;
}

static CborError
TcpsDecodeBlock(
    TCPS_LOG_BLOCK *Block,
    const uint8_t *EncodedBlock,
    size_t EncodedBlockSize)
{
    CborParser parser;
    CborValue itemIter, arrayIter;
    CborError err = CborNoError;
    uint8_t *buffer;
    size_t bufferSize;
    uint64_t valid64;

    if (Block == NULL ||
        EncodedBlock == NULL)
    {
        return CborErrorInternalError;
    }

    // open the encoded container
    CLEANUP_DECODER_ERR(cbor_parser_init(EncodedBlock, EncodedBlockSize, 0, &parser, &itemIter));
    if (!cbor_value_is_array(&itemIter))
    {
        err = CborErrorIllegalType;
        goto Cleanup;
    }
    CLEANUP_DECODER_ERR(cbor_value_enter_container(&itemIter, &arrayIter));

    // extract the initial chain hash
    if (!cbor_value_is_byte_string(&arrayIter))
    {
        err = CborErrorIllegalType;
        goto Cleanup;
    }
    CLEANUP_DECODER_ERR(cbor_value_ref_byte_string(&arrayIter, &buffer, &bufferSize, &arrayIter));
    if (bufferSize != sizeof(TCPS_SHA256_DIGEST))
    {
        err = CborErrorIllegalType;
        goto Cleanup;
    }
    memcpy(Block->InitialDigest, buffer, bufferSize);

    // extract the current chain hash
    if (!cbor_value_is_byte_string(&arrayIter))
    {
        err = CborErrorIllegalType;
        goto Cleanup;
    }
    CLEANUP_DECODER_ERR(cbor_value_ref_byte_string(&arrayIter, &buffer, &bufferSize, &arrayIter));
    if (bufferSize != sizeof(TCPS_SHA256_DIGEST))
    {
        err = CborErrorIllegalType;
        goto Cleanup;
    }
    memcpy(Block->CurrentDigest, buffer, bufferSize);

    // extract the category label (null-terminated)
    if (!cbor_value_is_byte_string(&arrayIter))
    {
        err = CborErrorIllegalType;
        goto Cleanup;
    }
    CLEANUP_DECODER_ERR(cbor_value_ref_byte_string(&arrayIter, (const uint8_t *)&Block->CategoryLabel, &bufferSize, &arrayIter));

    // extract the validation state
    if (!cbor_value_is_unsigned_integer(&arrayIter))
    {
        err = CborErrorIllegalType;
        goto Cleanup;
    }
    CLEANUP_DECODER_ERR(cbor_value_get_uint64(&arrayIter, &valid64));
    if (valid64 > UINT32_MAX)
    {
        err = CborErrorUnknownType;
        goto Cleanup;
    }
    Block->ValidationState = (TCPS_LOG_VALIDATION_STATE)valid64;
    CLEANUP_DECODER_ERR(cbor_value_advance_fixed(&arrayIter));

    // extract the block events payload
    if (!cbor_value_is_byte_string(&arrayIter))
    {
        err = CborErrorIllegalType;
        goto Cleanup;
    }
    CLEANUP_DECODER_ERR(cbor_value_ref_byte_string(&arrayIter, &Block->Payload, &Block->PayloadSize, &arrayIter));

    // close the encoded container
    CLEANUP_DECODER_ERR(TcpsCborDecodeEnd(&itemIter, &arrayIter));

Cleanup:
    return err;
}

static oe_result_t
TcpsLogWriteBlock(
    TCPS_LOG_ATTRIBUTES *LogAttributes,
    TCPS_LOG_CATEGORY *Category,
    const uint8_t *BlockPayload,
    size_t BlockPayloadSize)
{
    oe_result_t status = OE_OK;
    CborError err;
    TCPS_LOG_VALIDATION_STATE validationState = TCPS_LOG_VALIDATION_STATE_OK;
    uint8_t *encodedBlock;
    size_t encodedBlockSize;

    if (LogAttributes == NULL ||
        Category == NULL ||
        BlockPayload == NULL ||
        LogAttributes->RemoteTransport == NULL)
    {
        status = OE_INVALID_PARAMETER;
        goto Exit;
    }

    // TODO: determine validation state

    // encode the block
    err = TcpsEncodeBlock(
        Category,
        validationState,
        BlockPayload,
        BlockPayloadSize,
        encodedBlock,
        &encodedBlockSize);
    if (err == CborErrorOutOfMemory)
    {
        encodedBlock = (uint8_t *)malloc(encodedBlockSize);
        if (encodedBlock == NULL)
        {
            status = OE_OUT_OF_MEMORY;
            goto Exit;
        }
        err = TcpsEncodeBlock(
            Category,
            validationState,
            BlockPayload,
            BlockPayloadSize,
            encodedBlock,
            &encodedBlockSize);
    }
    if (err != CborNoError)
    {
        status = OE_FAILURE;
        goto Exit;
    }

    // write the block to the remote
    status = LogAttributes->RemoteTransport->WriteRemoteBlockHandler(
        LogAttributes->RemoteTransport->HandlerContext,
        Category->Label,
        encodedBlock,
        encodedBlockSize);
    if (status != OE_OK)
    {
        goto Exit;
    }

Exit:
    if (encodedBlock != NULL)
    {
        free(encodedBlock);
    }

    return status;
}

#pragma endregion Blocks

#pragma region Public

oe_result_t
TcpsLogOpen(
    _Outptr_ TCPS_LOG_ATTRIBUTES **LogAttributes,
    _In_ const TCPS_IDENTITY_PRIVATE *SigningIdentity,
    _In_ const TCPS_IDENTITY_PUBLIC *ValidationIdentity,
    _In_ PTCPS_LOG_TIME GetTimeHandler)
{
    oe_result_t status = OE_OK;

    if (LogAttributes == NULL ||
        SigningIdentity == NULL ||
        ValidationIdentity == NULL ||
        GetTimeHandler == NULL)
    {
        status = OE_INVALID_PARAMETER;
        goto Exit;
    }

    *LogAttributes = malloc(sizeof(TCPS_LOG_ATTRIBUTES));
    if (*LogAttributes == NULL)
    {
        status = OE_OUT_OF_MEMORY;
        goto Exit;
    }
    memset(*LogAttributes, 0, sizeof(TCPS_LOG_ATTRIBUTES));

    (*LogAttributes)->SigningIdentity = SigningIdentity;
    (*LogAttributes)->ValidationIdentity = ValidationIdentity;
    (*LogAttributes)->GetTimeHandler = GetTimeHandler;

Exit:
    if (status != OE_OK && *LogAttributes != NULL)
    {
        TcpsLogClose(*LogAttributes);
        *LogAttributes = NULL;
    }

    return status;
}

oe_result_t
TcpsLogAddCategory(
    _Inout_ TCPS_LOG_ATTRIBUTES *LogAttributes,
    _In_ const char *Label,
    _In_ const TCPS_SHA256_DIGEST Seed,
    _In_ PTCPS_LOG_CATEGORY_PERSIST PersistCategoryHandler,
    _In_ PTCPS_LOG_CATEGORY_RECOVER RecoverCategoryHandler,
    _In_ PTCPS_LOG_COUNTER_CREATE CreateCounterHandler,
    _In_ PTCPS_LOG_COUNTER_VALIDATE ValidateCounterHandler,
    _In_ PTCPS_LOG_COUNTER_INCREMENTGET IncrementGetCounterHandler,
    _In_ void *HandlerContext)
{
    oe_result_t status = OE_OK;
    CborError err;
    TCPS_LOG_CATEGORY *existing = NULL, *categories = NULL;
    TCPS_LOG_SIGNED_PAYLOAD signedEncodedCategory;
    uint8_t *encodedSignedPayload = NULL, *counterId = NULL, *counterValue = NULL;
    size_t encodedSignedPayloadSize = 0, counterIdSize = 0, counterValueSize = 0;
    bool supportsCounters = PersistCategoryHandler != NULL;

    if (LogAttributes == NULL ||
        Label == NULL ||
        Seed == NULL ||
        !((PersistCategoryHandler == NULL &&
           RecoverCategoryHandler == NULL &&
           CreateCounterHandler == NULL &&
           ValidateCounterHandler == NULL &&
           IncrementGetCounterHandler == NULL) ||
          (PersistCategoryHandler != NULL &&
           RecoverCategoryHandler != NULL &&
           CreateCounterHandler != NULL &&
           ValidateCounterHandler != NULL &&
           IncrementGetCounterHandler != NULL)))
    {
        status = OE_INVALID_PARAMETER;
        goto Exit;
    }

    TcpsGetCategory(LogAttributes, Label, &existing);
    if (existing != NULL)
    {
        status = OE_FAILURE;
        goto Exit;
    }

    categories = malloc(sizeof(TCPS_LOG_CATEGORY) * (LogAttributes->CategoryCount + 1));
    if (categories == NULL)
    {
        status = OE_OUT_OF_MEMORY;
        goto Exit;
    }
    memcpy(categories, LogAttributes->Categories, sizeof(TCPS_LOG_CATEGORY) * LogAttributes->CategoryCount);

    categories[LogAttributes->CategoryCount].Label = Label;
    categories[LogAttributes->CategoryCount].PersistCategoryHandler = PersistCategoryHandler;
    categories[LogAttributes->CategoryCount].RecoverCategoryHandler = RecoverCategoryHandler;
    categories[LogAttributes->CategoryCount].CreateCounterHandler = CreateCounterHandler;
    categories[LogAttributes->CategoryCount].ValidateCounterHandler = ValidateCounterHandler;
    categories[LogAttributes->CategoryCount].IncrementGetCounterHandler = IncrementGetCounterHandler;
    categories[LogAttributes->CategoryCount].HandlerContext = HandlerContext;

    if (supportsCounters)
    {
        status = RecoverCategoryHandler(
            HandlerContext,
            Label,
            &encodedSignedPayload,
            &encodedSignedPayloadSize);
        if (status != OE_OK)
        {
            goto Exit;
        }
    }

    if (encodedSignedPayload == NULL)
    {
        memcpy(categories[LogAttributes->CategoryCount].InitialDigest, Seed, sizeof(TCPS_SHA256_DIGEST));
        memcpy(categories[LogAttributes->CategoryCount].CurrentDigest, Seed, sizeof(TCPS_SHA256_DIGEST));

        if (supportsCounters)
        {
            status = CreateCounterHandler(
                HandlerContext,
                &counterId,
                &counterIdSize,
                &counterValue,
                &counterValueSize);
            if (status != OE_OK)
            {
                goto Exit;
            }

            categories[LogAttributes->CategoryCount].CounterId = counterId;
            categories[LogAttributes->CategoryCount].CounterIdSize = counterIdSize;

#ifdef TCPS_FREQUENT_COUNTERS
            status = TcpsPersistCategoryWithCounter(
                &categories[LogAttributes->CategoryCount],
                counterValue,
                counterValueSize,
                LogAttributes->ValidationIdentity,
                LogAttributes->SigningIdentity);
            if (status != OE_OK)
            {
                goto Exit;
            }
#endif
        }
    }
    else
    {
        err = TcpsDecodeSignedLogPayload(
            &signedEncodedCategory,
            &encodedSignedPayload,
            &encodedSignedPayloadSize);
        if (err != CborNoError)
        {
            status = OE_FAILURE;
            goto Exit;
        }

        status = TcpsValidateSignedLogPayload(&signedEncodedCategory);
        if (status != OE_OK)
        {
            goto Exit;
        }

        err = TcpsDecodeCategoryWithCounter(
            &categories[LogAttributes->CategoryCount],
            &counterValue,
            &counterValueSize,
            signedEncodedCategory.Payload,
            signedEncodedCategory.PayloadSize);
        if (err != CborNoError)
        {
            status = OE_FAILURE;
            goto Exit;
        }

        status = ValidateCounterHandler(
            HandlerContext,
            categories[LogAttributes->CategoryCount].CounterId,
            categories[LogAttributes->CategoryCount].CounterIdSize,
            counterValue,
            counterValueSize);
        if (status != OE_OK)
        {
            // TODO: handle this better - something malicious is probably happening
            goto Exit;
        }
    }

    if (LogAttributes->Categories != NULL)
    {
        free(LogAttributes->Categories);
    }
    LogAttributes->Categories = categories;
    LogAttributes->CategoryCount += 1;

Exit:
    if (status != OE_OK && categories != NULL)
    {
        free(categories);
    }
    if (encodedSignedPayload != NULL)
    {
        free(encodedSignedPayload);
    }
    if (counterValue != NULL)
    {
        free(counterValue);
    }

    return status;
}

oe_result_t
TcpsLogSetLocalTransport(
    _Inout_ TCPS_LOG_ATTRIBUTES *LogAttributes,
    _In_ PTCPS_LOG_LOCAL_WRITE WriteLocalEventHandler,
    _In_ PTCPS_LOG_LOCAL_READ ReadLocalBlockHandler,
    _In_ PTCPS_LOG_LOCAL_CLEAR ClearLocalBlockHandler,
    _In_ void *HandlerContext)
{
    oe_result_t status = OE_OK;
    TCPS_LOG_LOCAL_TRANSPORT *localTransport = NULL;

    if (LogAttributes == NULL ||
        WriteLocalEventHandler == NULL ||
        ReadLocalBlockHandler == NULL ||
        ClearLocalBlockHandler == NULL)
    {
        status = OE_INVALID_PARAMETER;
        goto Exit;
    }

    localTransport = malloc(sizeof(TCPS_LOG_LOCAL_TRANSPORT));
    if (localTransport == NULL)
    {
        status = OE_OUT_OF_MEMORY;
        goto Exit;
    }

    localTransport->WriteLocalEventHandler = WriteLocalEventHandler;
    localTransport->ReadLocalBlockHandler = ReadLocalBlockHandler;
    localTransport->ClearLocalBlockHandler = ClearLocalBlockHandler;
    localTransport->HandlerContext = HandlerContext;

    LogAttributes->LocalTransport = localTransport;

Exit:
    if (status != OE_OK && localTransport != NULL)
    {
        free(localTransport);
    }

    return status;
}

oe_result_t
TcpsLogSetRemoteTransport(
    _Inout_ TCPS_LOG_ATTRIBUTES *LogAttributes,
    _In_ PTCPS_LOG_REMOTE_WRITE WriteRemoteBlockHandler,
    _In_ void *HandlerContext)
{
    oe_result_t status = OE_OK;
    TCPS_LOG_REMOTE_TRANSPORT *remoteTransport = NULL;

    if (LogAttributes == NULL ||
        WriteRemoteBlockHandler == NULL)
    {
        status = OE_INVALID_PARAMETER;
        goto Exit;
    }

    remoteTransport = malloc(sizeof(TCPS_LOG_REMOTE_TRANSPORT));
    if (remoteTransport == NULL)
    {
        status = OE_OUT_OF_MEMORY;
        goto Exit;
    }

    remoteTransport->WriteRemoteBlockHandler = WriteRemoteBlockHandler;
    remoteTransport->HandlerContext = HandlerContext;

    LogAttributes->RemoteTransport = remoteTransport;

Exit:
    if (status != OE_OK && remoteTransport != NULL)
    {
        free(remoteTransport);
    }

    return status;
}

oe_result_t
TcpsLogWrite(
    _Inout_ TCPS_LOG_ATTRIBUTES *LogAttributes,
    _In_ const char *CategoryLabel,
    _In_ const uint8_t *Payload,
    _In_ size_t PayloadSize)
{
    oe_result_t status = OE_OK;
    CborError err;
    TCPS_LOG_CATEGORY *category;
    TCPS_LOG_EVENT event;
    TCPS_LOG_SIGNED_PAYLOAD signedEvent;
    uint8_t *encodedEvent = NULL, *encodedSignedEvent = NULL, *encodedVersionedEvent = NULL;
    size_t encodedEventSize = 0, encodedSignedEventSize = 0, encodedVersionedEventSize = 0;

    if (LogAttributes == NULL ||
        CategoryLabel == NULL ||
        Payload == NULL ||
        (LogAttributes->LocalTransport == NULL && LogAttributes->RemoteTransport == NULL))
    {
        status = OE_INVALID_PARAMETER;
        goto Exit;
    }

    // find the category
    TcpsGetCategory(LogAttributes, CategoryLabel, &category);
    if (category == NULL)
    {
        status = OE_FAILURE;
        goto Exit;
    }

    // create the event struct
    event.Payload = Payload;
    event.PayloadSize = PayloadSize;
    event.Timestamp = LogAttributes->GetTimeHandler(NULL);
    memcpy(event.LogChainDigest, category->CurrentDigest, sizeof(TCPS_SHA256_DIGEST));
    // TODO: get rid of this check and use a platform agnostic mechanism
#ifdef OE_USE_OPTEE
    event.Timestamp &= 0xFFFFFFFF; // optee TEE_Time has 32bit for seconds, get the right bits
#endif

    // encode the event
    err = TcpsEncodeLogEvent(&event, encodedEvent, &encodedEventSize);
    if (err == CborErrorOutOfMemory)
    {
        encodedEvent = (uint8_t *)malloc(encodedEventSize);
        if (encodedEvent == NULL)
        {
            status = OE_OUT_OF_MEMORY;
            goto Exit;
        }
        err = TcpsEncodeLogEvent(&event, encodedEvent, &encodedEventSize);
    }
    if (err != CborNoError)
    {
        status = OE_FAILURE;
        goto Exit;
    }

    // sign the event
    status = TcpsSignLogPayload(
        &signedEvent,
        encodedEvent,
        encodedEventSize,
        LogAttributes->ValidationIdentity,
        LogAttributes->SigningIdentity);
    if (status != OE_OK)
    {
        goto Exit;
    }

    err = TcpsEncodeSignedLogPayload(&signedEvent, encodedSignedEvent, &encodedSignedEventSize);
    if (err == CborErrorOutOfMemory)
    {
        encodedSignedEvent = (uint8_t *)malloc(encodedSignedEventSize);
        if (encodedSignedEvent == NULL)
        {
            status = OE_OUT_OF_MEMORY;
            goto Exit;
        }
        err = TcpsEncodeSignedLogPayload(&signedEvent, encodedSignedEvent, &encodedSignedEventSize);
    }
    if (err != CborNoError)
    {
        status = OE_FAILURE;
        goto Exit;
    }

    // version the event
    err = TcpsEncodeVersionedPayload(
        encodedSignedEvent,
        encodedSignedEventSize,
        encodedVersionedEvent,
        &encodedVersionedEventSize);
    if (err == CborErrorOutOfMemory)
    {
        encodedVersionedEvent = (uint8_t *)malloc(encodedVersionedEventSize);
        if (encodedVersionedEvent == NULL)
        {
            status = OE_OUT_OF_MEMORY;
            goto Exit;
        }
        err = TcpsEncodeVersionedPayload(
            encodedSignedEvent,
            encodedSignedEventSize,
            encodedVersionedEvent,
            &encodedVersionedEventSize);
    }
    if (err != CborNoError)
    {
        status = OE_FAILURE;
        goto Exit;
    }

    // update the last logged payload digest
    TcpsSHA256Block(Payload, PayloadSize, category->CurrentDigest);

    // write rollback prevention cookie
#ifdef TCPS_FREQUENT_COUNTERS
    if (category->PersistCategoryHandler != NULL)
    {
        status = TcpsPersistCategoryWithCounter(
            category,
            NULL,
            0,
            LogAttributes->ValidationIdentity,
            LogAttributes->SigningIdentity);
        if (status != OE_OK)
        {
            goto Exit;
        }
    }
#endif

    // persist the event
    if (LogAttributes->LocalTransport != NULL)
    {
        status = LogAttributes->LocalTransport->WriteLocalEventHandler(
            LogAttributes->LocalTransport->HandlerContext,
            CategoryLabel,
            encodedVersionedEvent,
            encodedVersionedEventSize);
    }
    else
    {
        status = TcpsLogWriteBlock(
            LogAttributes,
            category,
            encodedVersionedEvent,
            encodedVersionedEventSize);
    }
    if (status != OE_OK)
    {
        goto Exit;
    }

Exit:
    if (encodedEvent != NULL)
    {
        free(encodedEvent);
    }
    if (encodedSignedEvent != NULL)
    {
        free(encodedSignedEvent);
    }
    if (encodedVersionedEvent != NULL)
    {
        free(encodedVersionedEvent);
    }

    return status;
}

oe_result_t
TcpsLogFlush(
    _Inout_ TCPS_LOG_ATTRIBUTES *LogAttributes,
    _In_ const char *CategoryLabel)
{
    oe_result_t status = OE_OK;
    TCPS_LOG_CATEGORY *category;
    uint8_t *encodedEvents;
    size_t encodedEventsSize;

    if (LogAttributes == NULL ||
        CategoryLabel == NULL ||
        LogAttributes->RemoteTransport == NULL ||
        LogAttributes->LocalTransport == NULL)
    {
        status = OE_INVALID_PARAMETER;
        goto Exit;
    }

    // find the category
    TcpsGetCategory(LogAttributes, CategoryLabel, &category);
    if (category == NULL)
    {
        status = OE_FAILURE;
        goto Exit;
    }

    // read the block payload
    status = LogAttributes->LocalTransport->ReadLocalBlockHandler(
        LogAttributes->LocalTransport->HandlerContext,
        CategoryLabel,
        &encodedEvents,
        &encodedEventsSize);
    if (status != OE_OK)
    {
        goto Exit;
    }

    // write the block
    status = TcpsLogWriteBlock(
        LogAttributes,
        category,
        encodedEvents,
        encodedEventsSize);
    if (status != OE_OK)
    {
        goto Exit;
    }

    // clear the local cache
    status = LogAttributes->LocalTransport->ClearLocalBlockHandler(
        LogAttributes->LocalTransport->HandlerContext,
        CategoryLabel);
    if (status != OE_OK)
    {
        goto Exit;
    }

Exit:
    if (encodedEvents != NULL)
    {
        free(encodedEvents);
    }

    return status;
}

oe_result_t
TcpsLogClose(
    _Inout_ TCPS_LOG_ATTRIBUTES *LogAttributes)
{
    oe_result_t status = OE_OK;
    if (LogAttributes == NULL)
    {
        status = OE_INVALID_PARAMETER;
        goto Exit;
    }

    for (size_t i = 0; i < LogAttributes->CategoryCount; i++)
    {
#ifndef TCPS_FREQUENT_COUNTERS
        if (LogAttributes->Categories[i].PersistCategoryHandler != NULL)
        {
            status = (TcpsPersistCategoryWithCounter(
                          &LogAttributes->Categories[i],
                          NULL,
                          0,
                          LogAttributes->ValidationIdentity,
                          LogAttributes->SigningIdentity) == OE_OK &&
                      (status == OE_OK))
                         ? OE_OK
                         : OE_FAILURE;
        }
#endif

        if (LogAttributes->Categories[i].CounterId != NULL)
        {
            free(LogAttributes->Categories[i].CounterId);
        }
    }

    if (LogAttributes->Categories != NULL)
    {
        free(LogAttributes->Categories);
    }
    if (LogAttributes->LocalTransport != NULL)
    {
        free(LogAttributes->LocalTransport);
    }
    if (LogAttributes->RemoteTransport != NULL)
    {
        free(LogAttributes->RemoteTransport);
    }
    free(LogAttributes);

Exit:
    return status;
}

#pragma endregion Public
