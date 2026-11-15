// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "OwnerVerifiedRoot.h"
#include "RootOwnerAudit.h"
#include "RootWindowRead.h"

namespace SmUfsOwnerRead {
static const uint8_t expectedUuid[16] = {0x04,0x76,0x9d,0x26,0x2c,0xa0,0x48,0x25,0xb4,0x43,0xe2,0xe5,0xf8,0x83,0x00,0xf2};
static const uint8_t expectedImage[32] = {0xe5,0x63,0x4d,0x04,0xa8,0xac,0x35,0xf0,0x24,0x84,0x5f,0x58,0x33,0x94,0xe0,0xa3,0x5d,0xeb,0x26,0xa0,0x4c,0xb0,0xc0,0x02,0x86,0xa8,0x67,0x4e,0x25,0xa9,0xa4,0xf2};
static bool identity(const vr_view &v) {
 if (!v.hashes || v.image_bytes!=6077580800ULL || v.block_count!=370947 || v.generation!=1) return false;
 for(unsigned i=0;i<16;++i) if(v.volume_uuid[i]!=expectedUuid[i]) return false;
 for(unsigned i=0;i<32;++i) if(v.image_sha256[i]!=expectedImage[i]) return false;
 return true;
}
// All mutations are on a private, unpublished copy and are restored.
static uint32_t envelopeSelfTest(uint8_t *policy,size_t bytes,const uint8_t *key,
    const uint8_t *wrongUuidHeader,uint8_t *backup,const vr_crypto *crypto) {
 if(!policy||bytes<VR_HEADER_BYTES+VR_SIGNATURE_BYTES+32||!key||!wrongUuidHeader||!backup||!crypto)return 0;
 vr_view test{};uint32_t result=0;
 policy[VR_HEADER_BYTES]^=1;
 int status=vr_open(policy,bytes,key,1,crypto,&test);
 policy[VR_HEADER_BYTES]^=1;
 if(status==VR_SIGNATURE&&!test.hashes)result|=1;
 policy[VR_HEADER_BYTES+VR_SIGNATURE_BYTES]^=1;
 status=vr_open(policy,bytes,key,1,crypto,&test);
 policy[VR_HEADER_BYTES+VR_SIGNATURE_BYTES]^=1;
 if(status==VR_HASH_TABLE&&!test.hashes)result|=2;
 for(unsigned i=0;i<VR_HEADER_BYTES+VR_SIGNATURE_BYTES;++i){backup[i]=policy[i];policy[i]=wrongUuidHeader[i];}
 status=vr_open(policy,bytes,key,1,crypto,&test);
 if(status==VR_OK&&!identity(test))result|=4;
 for(unsigned i=0;i<VR_HEADER_BYTES+VR_SIGNATURE_BYTES;++i)policy[i]=backup[i];
 return result;
}
static bool blockSelfTest(const vr_view *view,uint64_t index,uint8_t *data,size_t bytes,const vr_crypto *crypto) {
 if(!data||!bytes)return false;
 data[0]^=1;int rejected=vr_verify_block(view,index,data,bytes,crypto);data[0]^=1;
 return rejected==VR_BLOCK&&vr_verify_block(view,index,data,bytes,crypto)==VR_OK;
}
struct Counters { uint64_t verified=0, failures=0; uint32_t selftests=0; };
struct Reader {
 const vr_view *view;const vr_crypto *crypto;Counters *counts;
 static constexpr uint64_t scratchBytes=VR_BLOCK_BYTES+SmUfsRoot::kPhysicalBlockBytes;
 template<class Physical>
 SmUfsRoot::ReadError read(Physical &physical,const SmUfsRoot::Layout &layout,const SmUfsRoot::Window &window,
     const SmUfsRoot::ReadRequest &request,uint8_t *scratch) {
  using SmUfsRoot::ReadError;
  if(!view||!crypto||!counts||!identity(*view)||window.size!=view->image_bytes||!scratch||
      !request.output||!request.bytes||!SmUfsRoot::rangeWithin(window,request.offset,request.bytes)) return ReadError::Argument;
  uint64_t copied=0;
  while(copied<request.bytes) {
   const uint64_t offset=request.offset+copied,index=offset/VR_BLOCK_BYTES,start=index*VR_BLOCK_BYTES;
   uint64_t amount=view->image_bytes-start;if(amount>VR_BLOCK_BYTES) amount=VR_BLOCK_BYTES;
   const auto status=SmUfsRoot::readWindowWithScratch(physical,layout,window,{start,amount,scratch},scratch+VR_BLOCK_BYTES);
   if(status!=ReadError::Success) return status;
   if(vr_verify_block(view,index,scratch,amount,crypto)!=VR_OK) {
    __atomic_add_fetch(&counts->failures,1ULL,__ATOMIC_RELAXED);return ReadError::Io;
   }
   if((__atomic_load_n(&counts->selftests,__ATOMIC_ACQUIRE)&8U)==0) {
    if(!blockSelfTest(view,index,scratch,amount,crypto)) {
     __atomic_add_fetch(&counts->failures,1ULL,__ATOMIC_RELAXED);return ReadError::Io;
    }
    __atomic_or_fetch(&counts->selftests,8U,__ATOMIC_RELEASE);
   }
   __atomic_add_fetch(&counts->verified,1ULL,__ATOMIC_RELAXED);
   const uint64_t within=offset-start;uint64_t take=amount-within;
   if(take>request.bytes-copied)take=request.bytes-copied;
   for(uint64_t i=0;i<take;++i)request.output[copied+i]=scratch[within+i];
   copied+=take;
  }
  return ReadError::Success;
 }
};
struct PlainReader {
 static constexpr uint64_t scratchBytes=SmUfsRoot::kPhysicalBlockBytes;
 template<class Physical>
 SmUfsRoot::ReadError read(Physical &p,const SmUfsRoot::Layout &l,const SmUfsRoot::Window &w,
     const SmUfsRoot::ReadRequest &r,uint8_t *scratch) { return SmUfsRoot::readWindowWithScratch(p,l,w,r,scratch); }
};
} // namespace SmUfsOwnerRead
