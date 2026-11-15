// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "Probe69Iobuf.h"

#include <sys/errno.h>
#include <sys/buf_internal.h>

static inline int
probe69_valid_dev(dev_t dev) __attribute__((const));

static inline int
probe69_valid_dev(dev_t dev)
{
    return dev != NODEV;
}

int
probe69_bind_iobuf(buf_t bp, dev_t dev)
{
    if (bp == NULL) {
        return EINVAL;
    }
    if (!probe69_valid_dev(dev)) {
        return ENXIO;
    }
    if (bp->b_lflags != (BL_BUSY | BL_IOBUF) ||
        bp->b_vp != NULL || bp->b_dev != NODEV || bp->b_flags != 0 ||
        bp->b_iodone != NULL || bp->b_upl != NULL || bp->b_error != 0 ||
        bp->b_resid != 0 || bp->b_bcount != 0 || bp->b_bufsize != 0) {
        return EBUSY;
    }

    /* This is the sole adapter store. Do not add setters or callbacks here. */
    bp->b_dev = dev;
    return 0;
}
