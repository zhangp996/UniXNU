// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once

#include <sys/buf.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Bind the audited device identity to a fresh buf_alloc(NULL) iobuf.
 * The caller owns allocation, exclusive lifetime, and the audited whitelist
 * device identity. Call immediately after buf_alloc(NULL), before any setters;
 * never call after submission or callback. The bridge enforces the initialized
 * fresh iobuf state and writes no buffer member except b_dev.
 */
int probe69_bind_iobuf(buf_t bp, dev_t dev);

#ifdef __cplusplus
}
#endif
