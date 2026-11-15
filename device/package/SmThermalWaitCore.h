// SPDX-License-Identifier: MIT
// Bounded, read-only kernel-stack walk shared by the native observer and host
// checks. The caller supplies a no-fault copy; this file never touches a map.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#define SMTW_FRAMES 8
enum {SMTW_OK=0,SMTW_RANGE=1,SMTW_READ=2,SMTW_ALIGN=3,SMTW_ORDER=4,SMTW_CAP=5};
static inline bool smtw_waiting(unsigned state,unsigned wait,unsigned run) {
  return (state&(wait|run))==wait;
}
/* read(context,address,out,size) returns true only for a complete no-fault copy. */
typedef bool (*smtw_read_fn)(void *context,uintptr_t address,void *out,size_t size);
/* One saved LR plus at most SMTW_FRAMES-1 chained return addresses, all inside
   [low,high). The chain ends on a zero next-fp; a non-increasing non-zero fp is
   reported as SMTW_ORDER rather than silently accepted. */
static inline unsigned smtw_kernel_walk(smtw_read_fn read,void *context,
 uintptr_t low,uintptr_t high,uint64_t lr,uint64_t fp,
 uint64_t frames[SMTW_FRAMES],unsigned *count) {
 *count=0;
 if(!read || !frames || !count)return SMTW_RANGE;
 if(!low || high<=low || high-low<2*sizeof(uint64_t))return SMTW_RANGE;
 if(lr)frames[(*count)++]=lr;
 if(!fp)return SMTW_OK;
 while(*count<SMTW_FRAMES) {
  if(fp&7U)return SMTW_ALIGN;
  if(fp<low || fp>high-2*sizeof(uint64_t))return SMTW_ORDER;
  uint64_t pair[2];
  if(!read(context,(uintptr_t)fp,pair,sizeof(pair)))return SMTW_READ;
  if(!pair[1])return SMTW_OK;
  frames[(*count)++]=pair[1];
  if(!pair[0])return SMTW_OK;
  if(pair[0]<=fp)return SMTW_ORDER;
  fp=pair[0];
 }
 return SMTW_CAP;
}
