// SPDX-License-Identifier: MIT
#include "SmTsensTemperatureWire.h"
#include <libkern/c++/OSObject.h>
#include <sys/errno.h>
extern "C" int native_sm8250_thermal_live_copy(unsigned,unsigned,int32_t *,uint64_t *);
// Qualified by the board builder against the same reference kernel.
extern "C" OSObject *sm_tsens_original_event_with_bytes(const void *,size_t);
namespace {
// Observation only: the last failure of the kernel-internal conversion path.
static unsigned last_error_code;
}
extern "C" unsigned native_sm8250_thermal_event_error(void) {
 return __atomic_load_n(&last_error_code,__ATOMIC_RELAXED);
}
// Return a retained original IOHIDEvent, or NULL. The caller owns the reference.
// This is a kernel-internal service callback, not a new user-client interface.
extern "C" OSObject *native_sm8250_thermal_copy_event(unsigned group,unsigned channel,
 unsigned event_type,uint64_t sender) {
 if(event_type!=15) {__atomic_store_n(&last_error_code,unsigned(EINVAL),__ATOMIC_RELAXED);return nullptr;}
 int32_t value;uint64_t timestamp;
 int error=native_sm8250_thermal_live_copy(group,channel,&value,&timestamp);
 if(error) {__atomic_store_n(&last_error_code,unsigned(error),__ATOMIC_RELAXED);return nullptr;}
 uint8_t wire[SmTsensTemperatureWireBytes];
 if(!sm_tsens_encode_temperature(wire,sizeof(wire),value,timestamp,sender)) {
  __atomic_store_n(&last_error_code,unsigned(ERANGE),__ATOMIC_RELAXED);return nullptr;
 }
 OSObject *event=sm_tsens_original_event_with_bytes(wire,sizeof(wire));
 __atomic_store_n(&last_error_code,event?0U:unsigned(ENOMEM),__ATOMIC_RELAXED);
 return event;
}
