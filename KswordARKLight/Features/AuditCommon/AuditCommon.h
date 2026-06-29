#pragma once

// AuditCommon.h is the small facade for KswordARKLight read-only audit UI
// helpers. Inputs are feature pages that need consistent status labels,
// formatting, tables or summary panels; processing is implemented in the
// split .cpp files; output is a reusable header that thread 13 can register
// without forcing every feature to include each helper individually.

#include "AuditFormatting.h"
#include "AuditStatus.h"
#include "AuditSummaryPanel.h"
#include "AuditTable.h"
