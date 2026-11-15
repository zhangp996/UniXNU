// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "RootOwnerRead.h"
#include "RootOwnerAudit.h"
#include <vm/vm_kern.h>
#include <vm/vm_map.h>
#include <mach/vm_prot.h>
extern "C" {
extern const vr_crypto vr_xnu_crypto;
extern const uint8_t smufs_owner_manifest[],smufs_owner_key[],smufs_owner_negative_header[];
extern const uint64_t smufs_owner_manifest_bytes,smufs_owner_negative_header_bytes;
}
namespace SmUfsOwnerPolicy {
static vr_view view;
static SmUfsOwnerRead::Counters counts;
static uint32_t initialized=0,signatureStatus=VR_FORMAT,immutable=0;
static const uint8_t *manifest=nullptr;static vm_offset_t allocation=0;static vm_size_t allocationBytes=0;
static bool requested() { uint32_t n=0;return PE_parse_boot_argn("native-ufs-owner-audit",&n,sizeof(n))&&n==1; }
static bool trustRequested() { uint32_t n=0;return PE_parse_boot_argn("native-ufs-owner-trust",&n,sizeof(n))&&n==1; }
static bool ready() { return __atomic_load_n(&initialized,__ATOMIC_ACQUIRE)==1; }
static bool initialize() {
 if(!requested())return true;
 if(ready())return true;
 if(smufs_owner_manifest_bytes!=11870848||smufs_owner_negative_header_bytes!=VR_HEADER_BYTES+VR_SIGNATURE_BYTES)return false;
 const vm_size_t bytes=round_page(smufs_owner_manifest_bytes+VR_KEY_BYTES+VR_HEADER_BYTES+VR_SIGNATURE_BYTES);vm_offset_t memory=0;
 if(kmem_alloc(kernel_map,&memory,bytes,static_cast<kma_flags_t>(KMA_DATA|KMA_ZERO),VM_KERN_MEMORY_IOKIT)!=KERN_SUCCESS)return false;
 uint8_t *policy=reinterpret_cast<uint8_t*>(memory);uint8_t *anchor=policy+smufs_owner_manifest_bytes;
 memcpy(policy,smufs_owner_manifest,smufs_owner_manifest_bytes);memcpy(anchor,smufs_owner_key,VR_KEY_BYTES);
 signatureStatus=vr_open(policy,smufs_owner_manifest_bytes,anchor,1,&vr_xnu_crypto,&view);
 if(signatureStatus!=VR_OK||!SmUfsOwnerRead::identity(view)) { view={};kmem_free(kernel_map,memory,bytes);return false; }
 const uint32_t tests=SmUfsOwnerRead::envelopeSelfTest(policy,smufs_owner_manifest_bytes,anchor,
     smufs_owner_negative_header,anchor+VR_KEY_BYTES,&vr_xnu_crypto);
 signatureStatus=vr_open(policy,smufs_owner_manifest_bytes,anchor,1,&vr_xnu_crypto,&view);
 if(tests!=7||signatureStatus!=VR_OK||!SmUfsOwnerRead::identity(view)) {
  __atomic_store_n(&counts.selftests,tests,__ATOMIC_RELEASE);view={};kmem_free(kernel_map,memory,bytes);return false;
 }
 __atomic_store_n(&counts.selftests,tests,__ATOMIC_RELEASE);
 if(vm_map_protect(kernel_map,memory,memory+bytes,VM_PROT_READ,FALSE)!=KERN_SUCCESS||
    !vm_map_check_protection(kernel_map,memory,memory+bytes,VM_PROT_READ)||
    vm_map_check_protection(kernel_map,memory,memory+bytes,VM_PROT_WRITE)) {
  view={};kmem_free(kernel_map,memory,bytes);return false;
 }
 // Immutable policy and public anchor intentionally live for this boot. No
 // signed data/key is published until successful protection and verification.
 manifest=policy;allocation=memory;allocationBytes=bytes;
 immutable=1;__atomic_store_n(&initialized,1U,__ATOMIC_RELEASE);return true;
}
static SmUfsOwnerRead::Reader reader() {return {ready()?&view:nullptr,&vr_xnu_crypto,&counts};}
}
