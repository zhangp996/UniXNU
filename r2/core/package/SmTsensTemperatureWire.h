// SPDX-License-Identifier: MIT
// Data layout derived from the pinned 21H461 withBytes reader and public format
// definitions. No Apple implementation is copied; the original reader is used.
#pragma once
#include <stdint.h>
#include <stddef.h>
enum { SmTsensTemperatureWireBytes=48 };
static inline void sm_tsens_put32(uint8_t *out,uint32_t value) {
 for(unsigned i=0;i<4;++i)out[i]=static_cast<uint8_t>(value>>(8*i));
}
static inline void sm_tsens_put64(uint8_t *out,uint64_t value) {
 for(unsigned i=0;i<8;++i)out[i]=static_cast<uint8_t>(value>>(8*i));
}
static inline bool sm_tsens_encode_temperature(uint8_t *out,size_t capacity,
 int32_t millicelsius,uint64_t timestamp,uint64_t sender) {
 // Exact representable range of this hardware's signed 12-bit 0.1C samples.
 if(!out || capacity<SmTsensTemperatureWireBytes || !timestamp ||
    millicelsius<-204800 || millicelsius>204700)return false;
 int32_t fixed=static_cast<int32_t>((static_cast<int64_t>(millicelsius)*65536)/1000);
 for(unsigned i=0;i<SmTsensTemperatureWireBytes;++i)out[i]=0;
 sm_tsens_put64(out,timestamp);sm_tsens_put64(out+8,sender);
 sm_tsens_put32(out+24,1); // one event; no attributes, options or child depth
 sm_tsens_put32(out+28,20);sm_tsens_put32(out+32,15);
 sm_tsens_put32(out+44,static_cast<uint32_t>(fixed));return true;
}
