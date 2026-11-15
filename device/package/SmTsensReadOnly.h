// SPDX-License-Identifier: MIT
// Clean board qualification logic. Register definitions: original kona DTB,
// Android cdeb8d4 kona.dtsi/tsens.h/tsens2xxx.c. No vendor code copied.
#ifndef SM_TSENS_READ_ONLY_H
#define SM_TSENS_READ_ONLY_H
#include <stdint.h>
struct SmTsensGroup {
 uint32_t ready_before,ready_after,valid;
 uint32_t raw[16];
 int32_t millicelsius[16];
};
typedef uint32_t (*SmTsensRead)(void *,unsigned);
static inline int32_t sm_tsens_decode(uint32_t raw) {
 int32_t deci=(int32_t)(raw&0xfffU);
 if(deci&0x800)deci-=0x1000;
 return deci*100;
}
// A bounded acquisition; no writes, polling, fallback sample or recovery.
static inline void sm_tsens_capture(SmTsensRead read,void *context,SmTsensGroup *out) {
 *out=SmTsensGroup{};
 out->ready_before=read(context,0xe4);
 if(!(out->ready_before&8U))return;
 for(unsigned i=0;i<16;++i) {
  out->raw[i]=read(context,0xa0+4*i);
  if(out->raw[i]&(1U<<21)) {
   out->valid|=1U<<i;
   out->millicelsius[i]=sm_tsens_decode(out->raw[i]);
  }
 }
 out->ready_after=read(context,0xe4);
 if(!(out->ready_after&8U))out->valid=0;
}
#endif
