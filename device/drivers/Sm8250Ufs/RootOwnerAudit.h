// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef SMUFS_OWNER_AUDIT_H
#define SMUFS_OWNER_AUDIT_H
#include <stdint.h>
enum { SMUFS_OWNER_SELFTEST_REQUIRED=15u };
struct SmUfsOwnerAudit {
 uint32_t enabled,policy,signature_status,immutable,bound,unique,uuid,typed,chain,readonly;
 uint32_t selftests,trust_requested;
 uint64_t units,verified,failures,media_id;
};
/* Darwin EAUTH=80 remains the Apple result. This is a separate owner decision. */
static inline uint32_t smufs_owner_accept_evidence(const struct SmUfsOwnerAudit *v,
    uint32_t selected,uint32_t mounted,uint32_t mount_readonly,int32_t apple_error)
{
 return v && apple_error==80 && mount_readonly==1 && selected==mounted && mounted!=0xffffffffU &&
     v->trust_requested==1 && v->enabled==1 && v->policy==1 && v->signature_status==0 &&
     v->immutable==1 && v->bound==1 && v->unique==1 && v->uuid==1 && v->typed==1 &&
     v->chain==1 && v->readonly==1 && v->selftests==SMUFS_OWNER_SELFTEST_REQUIRED &&
     v->verified>0 && v->failures==0;
}
#ifdef __cplusplus
extern "C" {
#endif
void smufs_owner_root_audit(uint32_t selected,uint32_t mounted,struct SmUfsOwnerAudit *out);
uint32_t smufs_owner_root_accept(uint32_t selected,uint32_t mounted,uint32_t mount_readonly,int32_t apple_error);
#ifdef __cplusplus
}
#endif
#endif
