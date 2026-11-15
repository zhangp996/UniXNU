// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "SystemOsPin.h"
#include "RootOwnerRead.h"

// Kernel-pinned SystemOS bytes, separate from the signed OS owner policy.
namespace SmUfsSystemRead {
static bool tableValid(const uint8_t *table,uint64_t bytes,const vr_crypto *crypto)
{
 if(!table||bytes!=SmUfsSystemPin::tableBytes||!crypto||!crypto->sha256)return false;
 uint8_t digest[32];return crypto->sha256(table,size_t(bytes),digest)&&!memcmp(digest,SmUfsSystemPin::tableDigest,32);
}
static bool windowValid(const SmUfsRoot::Window &w)
{
 return !strcmp(w.name,"SystemOS")&&w.offset==SmUfsSystemPin::windowOffset&&w.size==SmUfsSystemPin::imageBytes&&
     w.blockBytes==512&&w.flags==1&&!memcmp(w.digest,SmUfsSystemPin::imageDigest,32);
}
static bool blockValid(const uint8_t *table,uint64_t index,const uint8_t *data,uint64_t bytes,const vr_crypto *crypto)
{
 if(!table||index>=SmUfsSystemPin::units||!data||bytes!=SmUfsSystemPin::unitBytes||!crypto||!crypto->sha256)return false;
 uint8_t digest[32];return crypto->sha256(data,size_t(bytes),digest)&&!memcmp(digest,table+index*32,32);
}
static uint32_t policySelfTest(uint8_t *table,const SmUfsRoot::Window &original,const vr_crypto *crypto)
{
 uint32_t result=0;table[0]^=1;if(!tableValid(table,SmUfsSystemPin::tableBytes,crypto))result|=1;table[0]^=1;
 SmUfsRoot::Window wrong=original;wrong.digest[0]^=1;if(windowValid(original)&&!windowValid(wrong))result|=2;
 if(!blockValid(table,SmUfsSystemPin::units,table,SmUfsSystemPin::unitBytes,crypto))result|=4;
 return result;
}
static bool blockSelfTest(const uint8_t *table,uint64_t index,uint8_t *data,const vr_crypto *crypto)
{
 data[0]^=1;bool rejected=!blockValid(table,index,data,SmUfsSystemPin::unitBytes,crypto);data[0]^=1;
 return rejected&&blockValid(table,index,data,SmUfsSystemPin::unitBytes,crypto);
}
struct Reader {
 const uint8_t *table;const vr_crypto *crypto;SmUfsOwnerRead::Counters *counts;
 static constexpr uint64_t scratchBytes=SmUfsSystemPin::unitBytes+SmUfsRoot::kPhysicalBlockBytes;
 template<class Physical>
 SmUfsRoot::ReadError read(Physical &physical,const SmUfsRoot::Layout &layout,const SmUfsRoot::Window &window,
     const SmUfsRoot::ReadRequest &request,uint8_t *scratch)
 {
  using SmUfsRoot::ReadError;
  if(!table||!crypto||!counts||!scratch||!windowValid(window)||!request.output||!request.bytes||
      !SmUfsRoot::rangeWithin(window,request.offset,request.bytes))return ReadError::Argument;
  uint64_t copied=0;
  while(copied<request.bytes){
   const uint64_t offset=request.offset+copied,index=offset/SmUfsSystemPin::unitBytes,start=index*SmUfsSystemPin::unitBytes;
   const auto status=SmUfsRoot::readWindowWithScratch(physical,layout,window,
       {start,SmUfsSystemPin::unitBytes,scratch},scratch+SmUfsSystemPin::unitBytes);
   if(status!=ReadError::Success)return status;
   if(!blockValid(table,index,scratch,SmUfsSystemPin::unitBytes,crypto)){
    __atomic_add_fetch(&counts->failures,1ULL,__ATOMIC_RELAXED);return ReadError::Io;
   }
   if((__atomic_load_n(&counts->selftests,__ATOMIC_ACQUIRE)&8U)==0){
    if(!blockSelfTest(table,index,scratch,crypto)){
     __atomic_add_fetch(&counts->failures,1ULL,__ATOMIC_RELAXED);return ReadError::Io;
    }
    __atomic_or_fetch(&counts->selftests,8U,__ATOMIC_RELEASE);
   }
   __atomic_add_fetch(&counts->verified,1ULL,__ATOMIC_RELAXED);
   const uint64_t within=offset-start;uint64_t take=SmUfsSystemPin::unitBytes-within;
   if(take>request.bytes-copied)take=request.bytes-copied;
   memcpy(request.output+copied,scratch+within,size_t(take));copied+=take;
  }
  return ReadError::Success;
 }
};
}
