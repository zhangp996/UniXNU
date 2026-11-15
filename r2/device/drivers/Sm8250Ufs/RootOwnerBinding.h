// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
extern "C" __attribute__((noinline)) void
smufs_owner_root_audit(uint32_t selected,uint32_t mounted,SmUfsOwnerAudit *out)
{
 if(!out)return;*out={};out->enabled=SmUfsOwnerPolicy::requested();
 out->signature_status=SmUfsOwnerPolicy::signatureStatus;
 out->selftests=__atomic_load_n(&SmUfsOwnerPolicy::counts.selftests,__ATOMIC_ACQUIRE);
 out->trust_requested=SmUfsOwnerPolicy::trustRequested();
 out->policy=SmUfsOwnerPolicy::ready();out->immutable=SmUfsOwnerPolicy::immutable;
 out->units=out->policy?SmUfsOwnerPolicy::view.block_count:0;
 out->verified=__atomic_load_n(&SmUfsOwnerPolicy::counts.verified,__ATOMIC_ACQUIRE);
 out->failures=__atomic_load_n(&SmUfsOwnerPolicy::counts.failures,__ATOMIC_ACQUIRE);
 if(!out->enabled||!out->policy||selected!=mounted||mounted==0xffffffffU)return;
 OSDictionary *matching=IOService::serviceMatching("IOMedia");if(!matching)return;
 OSIterator *iterator=IOService::getMatchingServices(matching);matching->release();if(!iterator)return;
 uint32_t matches=0,visited=0;
 while(OSObject *object=iterator->getNextObject()) {
  if(++visited>256){out->unique=0;iterator->release();return;}
  IOMedia *media=static_cast<IOMedia*>(object->metaCast("IOMedia"));if(!media)continue;
  OSNumber *major=OSDynamicCast(OSNumber,media->getProperty("BSD Major"));
  OSNumber *minor=OSDynamicCast(OSNumber,media->getProperty("BSD Minor"));
  if(!major||!minor||major->unsigned32BitValue()!=((mounted>>24)&0xff)||minor->unsigned32BitValue()!=(mounted&0xffffff))continue;
  ++matches;out->media_id=media->getRegistryEntryID();
  OSString *uuid=OSDynamicCast(OSString,media->getProperty("UUID"));
  out->uuid=uuid&&uuid->isEqualTo("04769D26-2CA0-4825-B443-E2E5F88300F2");
  out->readonly=!media->isWritable();
  uint32_t whole=0,driver=0,typed=0;IORegistryEntry *entry=media;entry->retain();
  for(unsigned depth=0;entry&&depth<32;++depth) {
   if(OSDynamicCast(RootWholeMedia,entry))whole=1;
   if(OSDynamicCast(RootWindowDriver,entry))driver=1;
   RootWindowBlock *block=OSDynamicCast(RootWindowBlock,entry);
   if(block){typed=block->ownerAuditEligible();entry->release();entry=nullptr;break;}
   IORegistryEntry *next=entry->copyParentEntry(gIOServicePlane);entry->release();entry=next;
  }
  if(entry)entry->release();out->typed=typed;out->chain=whole&&driver&&typed;
 }
 iterator->release();out->unique=matches==1;
 out->bound=out->unique&&out->uuid&&out->readonly&&out->typed&&out->chain&&out->immutable&&out->verified&&!out->failures;
 // This function only gathers fresh evidence. Acceptance is separate below.
}

extern "C" __attribute__((noinline)) uint32_t
smufs_owner_root_accept(uint32_t selected,uint32_t mounted,uint32_t mount_readonly,int32_t apple_error)
{
 if(!SmUfsOwnerPolicy::trustRequested()||apple_error!=80||mount_readonly!=1)return 0;
 SmUfsOwnerAudit evidence;smufs_owner_root_audit(selected,mounted,&evidence);
 return smufs_owner_accept_evidence(&evidence,selected,mounted,mount_readonly,apple_error);
}

// Read-only diagnostic identity lookup. This does not authorize SystemOS execution.
extern "C" __attribute__((noinline)) uint32_t
smufs_cache_probe_device(uint32_t expected,uint32_t *device,char *path,uint64_t *media_id,uint32_t *matches)
{
 if(!device||!path||!media_id||!matches)return 0;
 *device=0xffffffffU;path[0]=0;*media_id=0;*matches=0;
 if(!SmUfsOwnerPolicy::ready()||!SmUfsOwnerPolicy::trustRequested())return 0;
 OSDictionary *matching=IOService::serviceMatching("IOMedia");if(!matching)return 0;
 OSIterator *iterator=IOService::getMatchingServices(matching);matching->release();if(!iterator)return 0;
 uint32_t visited=0,eligible=0;
 while(OSObject *object=iterator->getNextObject()) {
  if(++visited>256){iterator->release();return 0;}
  IOMedia *media=static_cast<IOMedia*>(object->metaCast("IOMedia"));if(!media||media->isInactive())continue;
  OSString *uuid=OSDynamicCast(OSString,media->getProperty("UUID"));
  if(!uuid||!uuid->isEqualTo("8846FE78-2910-4297-AF97-D59BB6F8FC16"))continue;
  ++*matches;
  OSNumber *major=OSDynamicCast(OSNumber,media->getProperty("BSD Major"));
  OSNumber *minor=OSDynamicCast(OSNumber,media->getProperty("BSD Minor"));
  OSString *name=OSDynamicCast(OSString,media->getProperty("BSD Name"));
  if(!major||!minor||!name||media->isWritable()||major->unsigned32BitValue()>255||minor->unsigned32BitValue()>0xffffff)continue;
  uint32_t dev=(major->unsigned32BitValue()<<24)|minor->unsigned32BitValue();
  if(expected!=0xffffffffU&&dev!=expected)continue;
  const char *bsd=name->getCStringNoCopy();size_t length=bsd?strlen(bsd):0;
  if(length<5||length>55||strncmp(bsd,"disk",4))continue;
  bool valid=true;for(size_t i=4;i<length;++i)if((bsd[i]<'0'||bsd[i]>'9')&&bsd[i]!='s')valid=false;
  if(!valid)continue;
  uint32_t whole=0,driver=0,typed=0;IORegistryEntry *entry=media;entry->retain();
  for(unsigned depth=0;entry&&depth<32;++depth) {
   if(OSDynamicCast(RootWholeMedia,entry))whole=1;
   if(OSDynamicCast(RootWindowDriver,entry))driver=1;
   RootWindowBlock *block=OSDynamicCast(RootWindowBlock,entry);
   if(block){typed=block->cacheProbeEligible();entry->release();entry=nullptr;break;}
   IORegistryEntry *next=entry->copyParentEntry(gIOServicePlane);entry->release();entry=next;
  }
  if(entry)entry->release();
  if(whole&&driver&&typed){++eligible;*device=dev;*media_id=media->getRegistryEntryID();snprintf(path,64,"/dev/%s",bsd);}
 }
 iterator->release();return *matches==1&&eligible==1;
}
