// SPDX-License-Identifier: MIT
// Board-lifetime readonly TSENS acquisition. HID publication is a separate
// consumer and must qualify its private ABI and channel identity first.
#include "SmTsensLiveCore.h"
#include "SmTsensHidState.h"
#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <kern/thread_call.h>
#include <machine/machine_routines.h>
#include <mach/vm_prot.h>
#include <sys/errno.h>
extern "C" vm_offset_t ml_io_map_with_prot(vm_offset_t,vm_size_t,vm_prot_t);
// Scalar snapshot published by the HID module; formatting happens here, in the
// timer thread, so the panic reader still only copies a prepared buffer.
extern "C" void native_sm8250_thermal_hid_state(SmTsensHidState *);
namespace {
static SmTsensLiveSnapshot latest;
static IOSimpleLock *snapshot_gate;
static thread_call_t timer;
static vm_offset_t maps[2];
static uint64_t maximum_age;
static unsigned prepared;
static unsigned note_sequence;
static char published_note[768];
// Used only by the existing panic renderer after interrupts are disabled.
static char panic_note[768];
static uint32_t read_status(void *opaque,unsigned offset) {
 vm_offset_t base=reinterpret_cast<vm_offset_t>(opaque);
 __asm__ volatile("dmb oshld" ::: "memory");
 uint32_t word=*reinterpret_cast<volatile const uint32_t *>(base+offset);
 __asm__ volatile("dmb oshld" ::: "memory");
 return word;
}
static void publish_note(const SmTsensLiveSnapshot &snapshot) {
 char text[768];
 SmTsensHidState hid;
 native_sm8250_thermal_hid_state(&hid);
 int n=snprintf(text,sizeof(text),"thermal-h4c live5 seq%llu tick%llu max-age%llu ro1 writes0\ntsens0 ready%x/%x valid%x\ntsens1 ready%x/%x valid%x\nhid step%s clserr%u raw%u,%u ep%u/2 req%u%u pub%u%u mt%u%u loc%x,%x g%u%u c%u%u prod%u/2 q%llu ok%llu off%llu bad%llu err%u kprobe%u%u\nqry0 n%llu t%u opt%x match%u pc%llx why%u q15=%llu ok%llu bad%llu\nqry1 n%llu t%u opt%x match%u pc%llx why%u q15=%llu ok%llu bad%llu\n",
  static_cast<unsigned long long>(snapshot.generation),static_cast<unsigned long long>(snapshot.timestamp),
  static_cast<unsigned long long>(maximum_age),snapshot.groups[0].ready_before,snapshot.groups[0].ready_after,snapshot.groups[0].valid,
  snapshot.groups[1].ready_before,snapshot.groups[1].ready_after,snapshot.groups[1].valid,
  hid.step,hid.class_error,hid.endpoint_error[0],hid.endpoint_error[1],hid.created,
  hid.requested[0],hid.requested[1],hid.published[0],hid.published[1],hid.matched[0],hid.matched[1],
  hid.location[0],hid.location[1],hid.group[0],hid.group[1],hid.channel[0],hid.channel[1],
  hid.product_ok[0]+hid.product_ok[1],
  static_cast<unsigned long long>(hid.queries),static_cast<unsigned long long>(hid.answered),
  static_cast<unsigned long long>(hid.offline),static_cast<unsigned long long>(hid.refused),hid.last_error,
  hid.event_probe[0],hid.event_probe[1],
  static_cast<unsigned long long>(hid.query[0].seen),hid.query[0].type,hid.query[0].options,hid.query[0].matching,
  static_cast<unsigned long long>(hid.query[0].caller),hid.query[0].reason,
  static_cast<unsigned long long>(hid.query[0].temperature),static_cast<unsigned long long>(hid.query[0].answered),
  static_cast<unsigned long long>(hid.query[0].rejected),
  static_cast<unsigned long long>(hid.query[1].seen),hid.query[1].type,hid.query[1].options,hid.query[1].matching,
  static_cast<unsigned long long>(hid.query[1].caller),hid.query[1].reason,
  static_cast<unsigned long long>(hid.query[1].temperature),static_cast<unsigned long long>(hid.query[1].answered),
  static_cast<unsigned long long>(hid.query[1].rejected));
 size_t length=n<0?0:static_cast<size_t>(n);if(length>=sizeof(text))length=sizeof(text)-1;text[length]=0;
 __atomic_add_fetch(&note_sequence,1U,__ATOMIC_SEQ_CST);
 for(size_t i=0;i<=length;++i)__atomic_store_n(&published_note[i],text[i],__ATOMIC_SEQ_CST);
 __atomic_add_fetch(&note_sequence,1U,__ATOMIC_SEQ_CST);
 // One normal-context print, outside all locks, exposes this boundary without
 // waiting for another eight-minute watchdog timeout. Panic keeps its own copy.
 if(snapshot.generation==30)printf("H4C query diagnostic (one shot)\n%s",text);
}
static void sample(thread_call_param_t,thread_call_param_t) {
 SmTsensGroup groups[2];
 for(unsigned g=0;g<2;++g)sm_tsens_capture(read_status,reinterpret_cast<void *>(maps[g]),&groups[g]);
 // Timestamp before publication; a reader's timestamp must be obtained while
 // holding the same gate so a concurrently newer publication cannot look like
 // clock rollback to that reader.
 uint64_t now=mach_absolute_time();
 bool interrupts=ml_set_interrupts_enabled(false);IOSimpleLockLock(snapshot_gate);
 sm_tsens_publish(&latest,groups,now);
 // Sole writer; no second callback is queued until after formatting finishes.
 IOSimpleLockUnlock(snapshot_gate);ml_set_interrupts_enabled(interrupts);
 publish_note(latest);
 uint64_t deadline;clock_interval_to_deadline(1,NSEC_PER_SEC,&deadline);
 thread_call_enter_delayed(timer,deadline);
}
}
extern "C" int native_sm8250_thermal_live_prepare(void) {
 if(__atomic_load_n(&prepared,__ATOMIC_ACQUIRE))return EALREADY;
 snapshot_gate=IOSimpleLockAlloc();if(!snapshot_gate)return ENOMEM;
 constexpr vm_offset_t physical[2]={0x0c263000,0x0c265000};
 for(unsigned g=0;g<2;++g) {
  maps[g]=ml_io_map_with_prot(physical[g],0x1ff,VM_PROT_READ);
  if(!maps[g]) {IOSimpleLockFree(snapshot_gate);snapshot_gate=nullptr;return ENOMEM;}
 }
 timer=thread_call_allocate(sample,nullptr);
 if(!timer) {IOSimpleLockFree(snapshot_gate);snapshot_gate=nullptr;return ENOMEM;}
 clock_interval_to_absolutetime_interval(3,NSEC_PER_SEC,&maximum_age);
 // Board-lifetime mappings/timer. No detach path is exposed by this provider.
 __atomic_store_n(&prepared,1U,__ATOMIC_RELEASE);sample(nullptr,nullptr);return 0;
}
extern "C" int native_sm8250_thermal_live_copy(unsigned group,unsigned channel,int32_t *value,uint64_t *timestamp) {
 if(!__atomic_load_n(&prepared,__ATOMIC_ACQUIRE))return ENXIO;
 bool interrupts=ml_set_interrupts_enabled(false);IOSimpleLockLock(snapshot_gate);
 auto result=sm_tsens_live_read(&latest,group,channel,mach_absolute_time(),maximum_age,value,timestamp);
 IOSimpleLockUnlock(snapshot_gate);ml_set_interrupts_enabled(interrupts);
 switch(result) {
  case SmTsensOk:return 0;
  case SmTsensInvalid:return EAGAIN;
  case SmTsensStale:return ETIMEDOUT;
  default:return EINVAL;
 }
}
extern "C" const char *native_sm8250_thermal_live_note(void) {
 // No lock, allocation, formatting or MMIO in panic context. Atomic character
 // loads avoid a C++ data race even if another CPU is formatting a new note.
 for(unsigned attempt=0;attempt<3;++attempt) {
  unsigned before=__atomic_load_n(&note_sequence,__ATOMIC_SEQ_CST);
  if(!before || (before&1U))continue;
  for(size_t i=0;i<sizeof(panic_note);++i) {
   char c=__atomic_load_n(&published_note[i],__ATOMIC_SEQ_CST);panic_note[i]=c;
   if(!c)break;
  }
  panic_note[sizeof(panic_note)-1]=0;
  if(before==__atomic_load_n(&note_sequence,__ATOMIC_SEQ_CST))return panic_note;
 }
 return "thermal-h4c snapshot unavailable\n";
}
