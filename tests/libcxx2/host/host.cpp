// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include <openenclave/host.h>
#include <openenclave/internal/tests.h>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "../args.h"

#if 0
#define ECHO
#endif

uint64_t prev;

void test(oe_enclave_t* enclave)
{
    oe_result_t r;
    args_t args;

    args.ret = -1;
    r = oe_call_enclave(enclave, "test", &args);
    printf("r=%u: %s\n", r, oe_result_str(r));
    OE_TEST(r == OE_OK);
    OE_TEST(args.ret == 0);
}

int main(int argc, const char* argv[])
{
    oe_result_t r;
    oe_enclave_t* enclave = NULL;

    if (argc != 2)
    {
        fprintf(stderr, "Usage: %s ENCLAVE\n", argv[0]);
        exit(1);
    }

    const uint32_t flags = oe_get_create_flags();
    const oe_enclave_type_t type = OE_ENCLAVE_TYPE_SGX;
    r = oe_create_enclave(argv[1], type, flags, NULL, 0, &enclave);
    OE_TEST(r == OE_OK);

    test(enclave);

    r = oe_terminate_enclave(enclave);
    OE_TEST(r == OE_OK);

    printf("=== passed all tests (%s)\n", argv[0]);

    return 0;
}
