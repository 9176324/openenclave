#ifndef _oe_host_blockdevice_h
#define _oe_host_blockdevice_h

#include <openenclave/host.h>

void oe_handle_open_block_device(
    oe_enclave_t* enclave,
    uint64_t arg_in,
    uint64_t* arg_out);

void oe_handle_close_block_device(oe_enclave_t* enclave, uint64_t arg_in);

void oe_handle_block_device_get(oe_enclave_t* enclave, uint64_t arg_in);

void oe_handle_block_device_put(oe_enclave_t* enclave, uint64_t arg_in);

#endif /* _oe_host_blockdevice_h */
