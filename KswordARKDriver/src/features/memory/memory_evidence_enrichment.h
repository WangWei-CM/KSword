#pragma once

#include "ark/ark_driver.h"

EXTERN_C_START

/*
 * Inputs: Row is an initialized memory evidence row with an optional bounded
 * sample/hash already filled by the scanner. Processing: the helper inspects
 * Row->sample when present, otherwise performs one bounded read-only prefix
 * probe for MZ/PE-like signatures and updates row/risk flags. Return behavior:
 * no value is returned; invalid or unreadable rows are left unchanged.
 */
VOID
KswordARKMemoryEvidenceApplyImageLikeHint(
    _Inout_ KSWORD_ARK_KERNEL_MEMORY_EVIDENCE_ROW* Row
    );

EXTERN_C_END
