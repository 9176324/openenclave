// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#ifndef _ASMDEFS_H
#define _ASMDEFS_H

#ifndef __ASSEMBLER__
#include <openenclave/internal/context.h>
#include <openenclave/bits/types.h>
#include <stdint.h>
#endif

#ifdef __ASSEMBLER__
#define ENCLU_EENTER 2
#define ENCLU_ERESUME 3
#endif

#define ThreadBinding_tcs 0
#define OE_WORDSIZE 8
#define OE_OCALL_CODE 3

#if defined(__linux__)
#define oe_enter __morestack
#endif

#ifndef __ASSEMBLER__
typedef struct _oe_enclave oe_enclave_t;
#endif

#ifndef __ASSEMBLER__
void oe_enter(
    void* tcs,
    void (*aep)(void),
    uint64_t arg1,
    uint64_t arg2,
    uint64_t* arg3,
    uint64_t* arg4,
    oe_enclave_t* enclave);

void OE_AEP(void);
#endif

#ifndef __ASSEMBLER__
void oe_enter_sim(
    void* tcs,
    void (*aep)(void),
    uint64_t arg1,
    uint64_t arg2,
    uint64_t* arg3,
    uint64_t* arg4,
    oe_enclave_t* enclave);
#endif

#ifndef __ASSEMBLER__
int __oe_dispatch_ocall(
    uint64_t arg1,
    uint64_t arg2,
    uint64_t* arg1Out,
    uint64_t* arg2Out,
    void* tcs,
    oe_enclave_t* enclave);
#endif

#ifndef __ASSEMBLER__
int _oe_host_stack_bridge(
    uint64_t arg1,
    uint64_t arg2,
    uint64_t* arg1Out,
    uint64_t* arg2Out,
    void* tcs,
    void* rsp);
#endif

#ifndef __ASSEMBLER__
typedef struct _oe_host_ocall_frame
{
    uint64_t previous_rbp;
    uint64_t return_address;
} oe_host_ocall_frame_t;
#endif

#ifndef __ASSEMBLER__
void _oe_notify_ocall_start(oe_host_ocall_frame_t* frame_pointer, void* tcs);
#endif

#ifndef __ASSEMBLER__
void _oe_notify_ocall_end(oe_host_ocall_frame_t* frame_pointer, void* tcs);
#endif

#ifndef __ASSEMBLER__
uint32_t _oe_push_enclave_instance(oe_enclave_t* enclave);
#endif

#ifndef __ASSEMBLER__
uint32_t _oe_remove_enclave_instance(oe_enclave_t* enclave);
#endif

#ifndef __ASSEMBLER__
oe_enclave_t* _oe_query_enclave_instance(void* tcs);
#endif
#endif /* _ASMDEFS_H */
