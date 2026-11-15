// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once

// Independent owner image-set identity. This is not Apple firmware personalization.
namespace SmUfsBootSet {
static const char domain[]="SMUFS.OwnerBootSet.v1";
static const uint8_t systemUuid[16]={0x88,0x46,0xfe,0x78,0x29,0x10,0x42,0x97,0xaf,0x97,0xd5,0x9b,0xb6,0xf8,0xfc,0x16};
static void u64(SHA384_CTX *context,uint64_t value)
{
 uint8_t bytes[8];for(unsigned i=0;i<8;++i)bytes[i]=uint8_t(value>>(8*i));
 SHA384_Update(context,bytes,sizeof(bytes));
}
// Encoding: NUL-terminated domain, role+kind (OS RSA3072-SHA256=1,
// SystemOS kernel pin=2), OS length+envelope SHA384;
// role+kind, SystemOS offset/size/unit/count/table-size (LE64), UUID, image
// SHA256, pinned table SHA256 and actual immutable table SHA384. No device
// numbers enter the stable identity; fresh device/media binding is a prerequisite.
static void encode(const uint8_t *manifest,uint64_t manifestBytes,const uint8_t *table,uint8_t digest[48])
{
 uint8_t rootDigest[48],systemDigest[48];SHA384_CTX context;
 SHA384_Init(&context);SHA384_Update(&context,manifest,size_t(manifestBytes));SHA384_Final(rootDigest,&context);
 SHA384_Init(&context);SHA384_Update(&context,table,size_t(SmUfsSystemPin::tableBytes));SHA384_Final(systemDigest,&context);
 const uint8_t rootRoleKind[2]={1,1},systemRoleKind[2]={2,2};
 SHA384_Init(&context);SHA384_Update(&context,domain,sizeof(domain));
 SHA384_Update(&context,rootRoleKind,sizeof(rootRoleKind));u64(&context,manifestBytes);SHA384_Update(&context,rootDigest,sizeof(rootDigest));
 SHA384_Update(&context,systemRoleKind,sizeof(systemRoleKind));
 u64(&context,SmUfsSystemPin::windowOffset);u64(&context,SmUfsSystemPin::imageBytes);u64(&context,SmUfsSystemPin::unitBytes);
 u64(&context,SmUfsSystemPin::units);u64(&context,SmUfsSystemPin::tableBytes);
 SHA384_Update(&context,systemUuid,sizeof(systemUuid));SHA384_Update(&context,SmUfsSystemPin::imageDigest,32);
 SHA384_Update(&context,SmUfsSystemPin::tableDigest,32);SHA384_Update(&context,systemDigest,sizeof(systemDigest));SHA384_Final(digest,&context);
}
}
extern "C" __attribute__((noinline)) uint32_t
smufs_owner_bootset_digest(uint32_t root,uint32_t system,int32_t apple_error,uint64_t expected_media,uint8_t digest[48])
{
 if(!digest)return 0;memset(digest,0,48);
 if(root==system||root==0xffffffffU||system==0xffffffffU||!expected_media||
    !SmUfsOwnerPolicy::ready()||!SmUfsOwnerPolicy::manifest||!SmUfsOwnerPolicy::immutable||
    !SmUfsOwnerRead::identity(SmUfsOwnerPolicy::view)||!SmUfsSystemPolicy::ready())return 0;
 SmUfsOwnerAudit evidence;smufs_owner_root_audit(root,root,&evidence);
 if(!smufs_owner_accept_evidence(&evidence,root,root,1,apple_error)||evidence.media_id==expected_media)return 0;
 if(!vm_map_check_protection(kernel_map,SmUfsOwnerPolicy::allocation,SmUfsOwnerPolicy::allocation+SmUfsOwnerPolicy::allocationBytes,VM_PROT_READ)||
    vm_map_check_protection(kernel_map,SmUfsOwnerPolicy::allocation,SmUfsOwnerPolicy::allocation+SmUfsOwnerPolicy::allocationBytes,VM_PROT_WRITE))return 0;
 uint64_t checked=0,bad=0;uint32_t tests=0;
 if(smufs_systemos_snapshot(&checked,&bad,&tests)!=15||!checked||bad||tests!=15)return 0;
 uint32_t selected=0xffffffffU,matches=0;uint64_t media=0;char path[64];
 if(!smufs_cache_probe_device(system,&selected,path,&media,&matches)||selected!=system||matches!=1||media!=expected_media)return 0;
 if(!SmUfsSystemRead::tableValid(SmUfsSystemPolicy::hashes,SmUfsSystemPin::tableBytes,&vr_xnu_crypto))return 0;
 SmUfsBootSet::encode(SmUfsOwnerPolicy::manifest,smufs_owner_manifest_bytes,SmUfsSystemPolicy::hashes,digest);return 1;
}
