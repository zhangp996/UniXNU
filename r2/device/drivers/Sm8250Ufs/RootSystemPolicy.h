// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "RootSystemRead.h"
extern "C" {
extern const uint8_t smufs_systemos_hashes[];
extern const uint64_t smufs_systemos_hash_bytes;
}
namespace SmUfsSystemPolicy {
static const uint8_t *hashes=nullptr;
static SmUfsOwnerRead::Counters counts;
static uint32_t initialized=0,immutable=0,pinOk=0;
static vm_offset_t allocation=0;static vm_size_t allocationBytes=0;
static bool requested(){uint32_t selector=0;return PE_parse_boot_argn("native-ufs-cache-probe",&selector,sizeof(selector))&&(selector==2||selector==3);}
static bool ready(){return __atomic_load_n(&initialized,__ATOMIC_ACQUIRE)==1;}
static bool initialize(){
 if(!requested())return true;
 if(ready())return true;
 if(!SmUfsOwnerPolicy::ready()||!SmUfsOwnerPolicy::trustRequested()||smufs_systemos_hash_bytes!=SmUfsSystemPin::tableBytes)return false;
 vm_size_t bytes=round_page(SmUfsSystemPin::tableBytes);vm_offset_t memory=0;
 if(kmem_alloc(kernel_map,&memory,bytes,static_cast<kma_flags_t>(KMA_DATA|KMA_ZERO),VM_KERN_MEMORY_IOKIT)!=KERN_SUCCESS)return false;
 uint8_t *copy=reinterpret_cast<uint8_t*>(memory);memcpy(copy,smufs_systemos_hashes,size_t(SmUfsSystemPin::tableBytes));
 if(!SmUfsSystemRead::tableValid(copy,SmUfsSystemPin::tableBytes,&vr_xnu_crypto)){kmem_free(kernel_map,memory,bytes);return false;}
 SmUfsRoot::Window window{};strlcpy(window.name,"SystemOS",sizeof(window.name));window.offset=SmUfsSystemPin::windowOffset;
 window.size=SmUfsSystemPin::imageBytes;window.flags=1;window.blockBytes=512;memcpy(window.digest,SmUfsSystemPin::imageDigest,32);
 uint32_t tests=SmUfsSystemRead::policySelfTest(copy,window,&vr_xnu_crypto);__atomic_store_n(&counts.selftests,tests,__ATOMIC_RELEASE);
 if(tests!=7||!SmUfsSystemRead::tableValid(copy,SmUfsSystemPin::tableBytes,&vr_xnu_crypto)) {kmem_free(kernel_map,memory,bytes);return false;}
 if(vm_map_protect(kernel_map,memory,memory+bytes,VM_PROT_READ,FALSE)!=KERN_SUCCESS||
    !vm_map_check_protection(kernel_map,memory,memory+bytes,VM_PROT_READ)||
    vm_map_check_protection(kernel_map,memory,memory+bytes,VM_PROT_WRITE)){kmem_free(kernel_map,memory,bytes);return false;}
 // Publish only the restored, pinned, read-only private copy; retain for this boot.
 hashes=copy;allocation=memory;allocationBytes=bytes;immutable=1;pinOk=1;
 __atomic_store_n(&initialized,1U,__ATOMIC_RELEASE);return true;
}
static SmUfsSystemRead::Reader reader(){return {ready()?hashes:nullptr,&vr_xnu_crypto,&counts};}
}
extern "C" __attribute__((noinline)) uint32_t
smufs_systemos_snapshot(uint64_t *verified,uint64_t *failures,uint32_t *tests)
{
 if(!verified||!failures||!tests)return 0;
 bool published=SmUfsSystemPolicy::ready();
 *verified=__atomic_load_n(&SmUfsSystemPolicy::counts.verified,__ATOMIC_ACQUIRE);
 *failures=__atomic_load_n(&SmUfsSystemPolicy::counts.failures,__ATOMIC_ACQUIRE);
 *tests=__atomic_load_n(&SmUfsSystemPolicy::counts.selftests,__ATOMIC_ACQUIRE);
 bool readonly=published&&SmUfsSystemPolicy::immutable&&
     vm_map_check_protection(kernel_map,SmUfsSystemPolicy::allocation,SmUfsSystemPolicy::allocation+SmUfsSystemPolicy::allocationBytes,VM_PROT_READ)&&
     !vm_map_check_protection(kernel_map,SmUfsSystemPolicy::allocation,SmUfsSystemPolicy::allocation+SmUfsSystemPolicy::allocationBytes,VM_PROT_WRITE);
 return (SmUfsSystemPolicy::requested()?1U:0U)|(published?2U:0U)|(readonly?4U:0U)|(SmUfsSystemPolicy::pinOk?8U:0U);
}
