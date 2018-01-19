#ifndef _ASMDEFS_H
#define _ASMDEFS_H

#ifndef __ASSEMBLER__
#include <openenclave/enclave.h>
#endif

#define ENCLU_EENTER 2
#define ENCLU_EEXIT 4

#define PAGE_SIZE 4096
#define STATIC_STACK_SIZE 8*100
#define OE_WORD_SIZE 8

#define CODE_ERET 0x200000000

/* Offsets into TD structure */
#define TD_self_addr            0
#define TD_last_sp              8
#define TD_magic                152
#define TD_depth                160
#define TD_initialized          168
#define TD_host_rcx             176
#define TD_host_rdx             184
#define TD_host_r8              192
#define TD_host_r9              200
#define TD_host_r10             208
#define TD_host_r11             216
#define TD_host_r12             224
#define TD_host_r13             232
#define TD_host_r14             240
#define TD_host_r15             248
#define TD_host_rsp             256
#define TD_host_rbp             264
#define TD_host_stack_base      272
#define TD_oret_func            280
#define TD_oret_arg             288
#define TD_callsites            296
#define TD_simulate             304

#define OE_Exit __morestack
#ifndef __ASSEMBLER__
void OE_Exit(uint64_t arg1, uint64_t arg2);
#endif

#ifndef __ASSEMBLER__
void __OE_HandleMain(
    uint64_t arg1,
    uint64_t arg2,
    uint64_t cssa,
    void* tcs);
#endif

#ifndef __ASSEMBLER__
void _OE_NotifyNestedExistStart(
    uint64_t arg1,
    OE_OCallContext* ocallContext
);
#endif
#endif /* _ASMDEFS_H */
