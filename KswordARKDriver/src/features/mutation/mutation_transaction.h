#pragma once

#include "ark/ark_mutation.h"

EXTERN_C_START

/*
 * Inputs: none. Processing: exposes the fixed response size used by mutation
 * handlers. Return: macro expands to the byte size of KSWORD_ARK_MUTATION_RESPONSE.
 */
#define KSWORD_ARK_MUTATION_RESPONSE_FIXED_SIZE sizeof(KSWORD_ARK_MUTATION_RESPONSE)

/*
 * Inputs: none. Processing: strips the flexible audit entry from the response
 * structure size. Return: macro expands to the minimum audit response byte size.
 */
#define KSWORD_ARK_MUTATION_AUDIT_RESPONSE_HEADER_SIZE \
    (sizeof(KSWORD_ARK_MUTATION_QUERY_AUDIT_RESPONSE) - sizeof(KSWORD_ARK_MUTATION_AUDIT_ENTRY))

EXTERN_C_END
