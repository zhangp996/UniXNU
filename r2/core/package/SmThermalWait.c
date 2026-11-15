// SPDX-License-Identifier: MIT
// Board diagnostic: bounded kernel-side wait observer. Uses the GF fixed
// 88da12ad blocked-kernel-stack pattern with this XNU's native types; no
// upstream implementation or target address is copied.
#include <kern/task.h>
#include <vm/vm_map.h>
#include <pexpert/pexpert.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <kern/spl.h>
#include <kern/thread_call.h>
#include <kern/clock.h>
#include <kern/misc_protos.h>
#include <arm/thread.h>
#include <machine/machine_routines.h>
#include <mach/arm/thread_status.h>
#include <string.h>
#include <stdarg.h>
#include "SmThermalWaitCore.h"
#include "SmThermalWaitBridge.h"
#include "SmGateTrace.h"
#include "SmThermalWaitExpected.h"
// Exact native declaration from bsd/libkern/libkern.h. Including that BSD
// private header in this Mach translation unit conflicts with Mach's types.
extern int vsnprintf(char *,size_t,const char *,va_list) __printflike(3,0);
// Panic renderer accepts 1023 bytes: reserve space for phase and live summary
// even when the snapshot is full. Main registers precede optional worker lines.
enum {SMTW_THREADS=32,SMTW_TEXT=768,SMTW_PHASE_TEXT=128};
struct smtw_site {
 uint64_t tid,pc,lr,fp,esr,far,wait,continuation,kframes[SMTW_FRAMES];
 uint64_t args[8],trap,sp,uframes[16];
 unsigned state,tag,why,count,kcount,kstop;
};
struct smtw_sample {
 unsigned attempt,scanned,total,kept,main_count,main_index,identity_error,uuid_source;
 int pid;unsigned uid;uint64_t tick,base,taskid;uint32_t slide;
 uint8_t cache_uuid[16],exec_uuid[16];
 struct smtw_site sites[SMTW_THREADS];
};
struct smtw_progress {
 unsigned polls,samples,misses,last_scan,last_match,rearm;
 int last_pid;bool done;
};
static struct smtw_sample smtw_sample;
static struct smtw_progress smtw_progress;
static thread_call_t smtw_call;
static unsigned smtw_started;
// Two independent publications: the last kernel snapshot survives later phase
// updates, so a phase line can never be mistaken for the sample it replaced.
static char smtw_snapshot[SMTW_TEXT];static unsigned smtw_snapshot_seq;
static char smtw_phase[SMTW_PHASE_TEXT];static unsigned smtw_phase_seq;
extern const char *native_sm8250_thermal_live_note(void);
extern unsigned native_sm_gop_status(unsigned);
int native_sm8250_thermal_wait_prepare(void);
const char *native_sm8250_thermal_wait_note(void);

static void smtw_store(char *destination,size_t capacity,unsigned *sequence,const char *text) {
 size_t length=strnlen(text,capacity-1);
 __atomic_add_fetch(sequence,1U,__ATOMIC_SEQ_CST);
 for(size_t i=0;i<=length;++i)__atomic_store_n(&destination[i],text[i],__ATOMIC_SEQ_CST);
 __atomic_add_fetch(sequence,1U,__ATOMIC_SEQ_CST);
}
static void smtw_publish(const char *text) {
 smtw_store(smtw_snapshot,SMTW_TEXT,&smtw_snapshot_seq,text);
}
static int smtw_format_into(char *out,size_t capacity,const char *format,...) __printflike(3,4);
static int smtw_format_into(char *out,size_t capacity,const char *format,...) {
 va_list args;va_start(args,format);int count=vsnprintf(out,capacity,format,args);va_end(args);return count;
}
static void smtw_phase_step(const char *step,unsigned attempt) {
 char text[SMTW_PHASE_TEXT];
 int count=smtw_format_into(text,sizeof(text),"thermal-wait3 phase%s attempt%u tick%llx\n",
  step,attempt,(unsigned long long)mach_absolute_time());
 if(count>0)smtw_store(smtw_phase,SMTW_PHASE_TEXT,&smtw_phase_seq,text);
}
// Panic context only: bounded atomic copies, no lock, allocation, formatting,
// VM read or MMIO. Normal worker code never calls this getter.
static size_t smtw_copy(char *out,size_t room,const char *source,size_t capacity,
 unsigned *sequence,bool *stable) {
 *stable=false;
 if(!room)return 0;
 for(unsigned retry=0;retry<3;++retry) {
  unsigned before=__atomic_load_n(sequence,__ATOMIC_SEQ_CST);
  if(before&1U)continue;
  size_t used=0;
  while(used+1<room && used+1<capacity) {
   char character=__atomic_load_n(&source[used],__ATOMIC_SEQ_CST);
   out[used++]=character;if(!character) {--used;break;}
  }
  out[used]=0;
  if(before==__atomic_load_n(sequence,__ATOMIC_SEQ_CST)) {*stable=true;return used;}
 }
 return 0;
}
const char *native_sm8250_thermal_wait_note(void) {
 static char note[1024];
 bool stable=false;size_t used=smtw_copy(note,sizeof(note),smtw_snapshot,SMTW_TEXT,&smtw_snapshot_seq,&stable);
 if(!stable) {for(size_t i=0;i<sizeof(note);++i)note[i]=0;used=0;}
 if(used && used<sizeof(note)-1 && note[used-1]!='\n')note[used++]='\n';
 bool phase_stable=false;
 used+=smtw_copy(note+used,sizeof(note)-used,smtw_phase,SMTW_PHASE_TEXT,&smtw_phase_seq,&phase_stable);
 if(!phase_stable && used<sizeof(note)-1) {
  const char *label="phase unavailable (bounded read refused)\n";
  size_t i=0;while(label[i] && used<sizeof(note)-1)note[used++]=label[i++];
 }
 if(used && used<sizeof(note)-1 && note[used-1]!='\n')note[used++]='\n';
 // Preserve only the first live-temperature summary line; both getters are
 // panic-only and the worker never calls either.
 const char *thermal=native_sm8250_thermal_live_note();
 for(unsigned i=0;i<126 && thermal[i] && thermal[i]!='\n' && used<sizeof(note)-1;++i)note[used++]=thermal[i];
 note[used]=0;return note;
}
static bool smtw_kernel_read(void *context,uintptr_t address,void *out,size_t size) {
 (void)context;
 return ml_nofault_copy((vm_offset_t)address,(vm_offset_t)out,size)==size;
}

static const uint8_t smui_exec_uuid[3][16]={{0x97,0x49,0x8f,0x96,0xfa,0x26,0x38,0xc9,0xb5,0xe5,0xf8,0xa2,0xae,0x58,0x31,0x06},{0xa0,0x7c,0x4b,0x68,0xd9,0x6d,0x32,0xce,0x98,0xd8,0x01,0x80,0x75,0x58,0x54,0x71},{0x1a,0xb6,0xa2,0x83,0x69,0x5a,0x3e,0x4a,0xa3,0xd4,0x2e,0x7a,0xa2,0xf2,0xdd,0x42}};
static const uint8_t smui_cache_uuid[16]={0x59,0x0d,0x83,0x9a,0x82,0xd2,0x30,0xf1,0xa1,0x7d,0x0c,0xc0,0x53,0x42,0x0f,0x73};
static void
smui_user_frames(task_t task,thread_t thread,struct smtw_site *site)
{
 const uint64_t limit=0x0000800000000000ULL;
 uint64_t fp=site->fp;
 if(site->why || !site->sp || site->sp>=limit-65536)return;
 vm_map_t map=get_task_map_reference(task);
 if(!map)return;
 vm_map_t old=vm_map_switch(map);
 for(unsigned i=0;i<16;++i) {
  uint64_t pair[2];
  if((fp&15) || fp<site->sp || fp>site->sp+65536-sizeof(pair) ||
     copyin((user_addr_t)fp,pair,sizeof(pair)))break;
  if(!pair[1] || pair[1]>=limit || (pair[1]&3))break;
  site->uframes[site->count++]=pair[1];
  if(pair[0]<=fp)break;
  fp=pair[0];
 }
 vm_map_switch(old);
 vm_map_deallocate(map);
 // Reject the frame chain if the saved site changed during the unlocked read.
 spl_t priority=splsched();
 thread_lock(thread);
 const arm_saved_state_t *source=get_user_regs(thread);
 arm_saved_state_t saved;
 if(!smtw_waiting(thread->state,TH_WAIT,TH_RUN) || thread->wait_event!=site->wait ||
    !source || ml_nofault_copy((vm_offset_t)source,(vm_offset_t)&saved,sizeof(saved))!=sizeof(saved) ||
    !is_saved_state64(&saved) || saved.ash.count!=ARM_SAVED_STATE64_COUNT ||
    (uint64_t)get_saved_state_pc(&saved)!=site->pc || (uint64_t)get_saved_state_sp(&saved)!=site->sp ||
    (uint64_t)get_saved_state_fp(&saved)!=site->fp)site->count=0;
 thread_unlock(thread);
 splx(priority);
}


static void smtw_capture(task_t task,struct smtw_sample *s,bool ui,bool all_frames) {
 thread_t retained[SMTW_THREADS],thread;unsigned kept=0;
 task_lock(task);
 if(task->active) {
  s->total=task->thread_count;s->base=task->mach_header_vm_address;
  s->taskid=task->task_uniqueid;s->slide=task->task_shared_region_slide;
  memcpy(s->cache_uuid,task->task_shared_region_uuid,sizeof(s->cache_uuid));
  queue_iterate(&task->threads,thread,thread_t,task_threads) {
   if(kept==SMTW_THREADS)break;
   thread_reference(thread);retained[kept++]=thread;
  }
 }
 task_unlock(task);s->kept=kept;s->main_index=SMTW_THREADS;
 for(unsigned i=0;i<kept;++i) {
  thread=retained[i];struct smtw_site *site=&s->sites[i];
  spl_t priority=splsched();thread_lock(thread);
  site->tid=thread_tid(thread);site->state=thread->state;site->tag=thread_get_tag(thread);
  site->wait=thread->wait_event;site->continuation=(uint64_t)thread->continuation;
  site->why=1; // running/not waiting: no saved-state read
  if(smtw_waiting(site->state,TH_WAIT,TH_RUN)) {
   const arm_saved_state_t *source=get_user_regs(thread);arm_saved_state_t saved;
   site->why=2;
   if(source && ml_nofault_copy((vm_offset_t)source,(vm_offset_t)&saved,sizeof(saved))==sizeof(saved)) {
    site->why=3;
    if(is_saved_state64(&saved) && saved.ash.count==ARM_SAVED_STATE64_COUNT) {
     site->pc=get_saved_state_pc(&saved);site->lr=get_saved_state_lr(&saved);
     site->fp=get_saved_state_fp(&saved);
     site->sp=get_saved_state_sp(&saved);
     site->esr=get_saved_state_esr(&saved);site->far=get_saved_state_far(&saved);
     // Only the already-copied kernel saved state is accessed. These are raw
     // registers, not a decoded message or evidence of the current IPC phase.
     site->trap=get_saved_state_reg(&saved,16);
     for(unsigned a=0;a<8;++a)site->args[a]=get_saved_state_reg(&saved,a);
     site->why=0;
    }
   }
   if(site->why) site->kstop=4;
   else {
    uintptr_t low=(uintptr_t)thread->kernel_stack,high=0;
    if(thread->continuation!=NULL) site->kstop=1;
    else if(!low || !kernel_stack_size || kernel_stack_size<sizeof(struct thread_kernel_state) ||
     low>(uintptr_t)-1-kernel_stack_size) site->kstop=2;
    else {
     high=low+kernel_stack_size;
     {
      arm_kernel_saved_state_t kernel_saved;
      uintptr_t from=(uintptr_t)&thread_get_kernel_state(thread)->machine.ss;
      if(ml_nofault_copy((vm_offset_t)from,(vm_offset_t)&kernel_saved,sizeof(kernel_saved))!=sizeof(kernel_saved))
       site->kstop=3;
      else site->kstop=smtw_kernel_walk(smtw_kernel_read,NULL,low,high,
       kernel_saved.lr,kernel_saved.fp,site->kframes,&site->kcount);
     }
    }
   }
  } else site->kstop=5;
  thread_unlock(thread);splx(priority);
  if(ui && !memcmp(s->cache_uuid,smui_cache_uuid,16) && (all_frames || i<12 || (site->tag&THREAD_TAG_MAINTHREAD)))smui_user_frames(task,thread,site);
  if(site->tag&THREAD_TAG_MAINTHREAD) {++s->main_count;s->main_index=i;}
  thread_deallocate(thread);
 }
 task_lock(task);
 bool changed=!task->active || task->task_uniqueid!=s->taskid ||
  task->mach_header_vm_address!=s->base || task->task_shared_region_slide!=s->slide ||
  memcmp(task->task_shared_region_uuid,s->cache_uuid,16);
 task_unlock(task);
 if(changed) {
  s->identity_error=11;
  for(unsigned i=0;i<kept;++i) {
   s->sites[i].pc=0;s->sites[i].lr=0;s->sites[i].fp=0;s->sites[i].esr=0;s->sites[i].far=0;
   s->sites[i].why=4;s->sites[i].kcount=0;s->sites[i].kstop=6;
   s->sites[i].trap=0;memset(s->sites[i].args,0,sizeof(s->sites[i].args));
  }
 }
}
struct smtw_text {char data[SMTW_TEXT];size_t used;bool cut;};
static void smtw_append(struct smtw_text *,const char *,...) __printflike(2,3);
static void smtw_append(struct smtw_text *text,const char *format,...) {
 if(text->cut)return;
 va_list args;va_start(args,format);
 int count=vsnprintf(text->data+text->used,sizeof(text->data)-text->used,format,args);va_end(args);
 if(count<0 || (size_t)count>=sizeof(text->data)-text->used) {text->cut=true;return;}
 text->used+=(size_t)count;
}
static void smtw_hex(char out[33],const uint8_t uuid[16]) {
 const char *digits="0123456789abcdef";
 for(unsigned i=0;i<16;++i) {out[2*i]=digits[uuid[i]>>4];out[2*i+1]=digits[uuid[i]&15];}out[32]=0;
}
static void smtw_format(struct smtw_sample *s) {
 struct smtw_text text={0};char eu[33],cu[33];smtw_hex(eu,s->exec_uuid);smtw_hex(cu,s->cache_uuid);
 smtw_append(&text,"display20 stage%u copy%u retire%u (worker snapshot)\n",
  native_sm_gop_status(0),native_sm_gop_status(3),native_sm_gop_status(4));
 smtw_append(&text,"thermal-wait3 poll%u sample%u miss%u scan%u match%u pid%d done%u rearm%u\n",
  smtw_progress.polls,smtw_progress.samples,smtw_progress.misses,smtw_progress.last_scan,
  smtw_progress.last_match,smtw_progress.last_pid,smtw_progress.done?1U:0U,smtw_progress.rearm);
 if(!smtw_progress.samples) {
  smtw_append(&text,"no referenced target; bounded waiting\n");
  smtw_publish(text.data);return;
 }
 smtw_append(&text,"snapshot n%u tick%llx pid%d uid%u task%llx scan%u threads%u kept%u drop%u main%u iderr%u uuid%d best1\n",
  s->attempt,s->tick,s->pid,s->uid,s->taskid,s->scanned,s->total,s->kept,
  s->total>=s->kept?s->total-s->kept:0,s->main_count,s->identity_error,s->uuid_source?1U:0U);
 smtw_append(&text,"ex %llx sl%x eu%c%c%c%c cu%c%c%c%c\n",s->base,s->slide,
  eu[0],eu[1],eu[2],eu[3],cu[0],cu[1],cu[2],cu[3]);
 struct SmGateFields gate;
 int gate_state=smgate_read(&gate);
 if(gate_state<0) smtw_append(&text,"gate invalid (dropped transition)\n");
 else if(!gate_state) smtw_append(&text,"gate unstable\n");
 else {
  uint64_t gate_now=mach_absolute_time();
  uint32_t conflict=(gate.requester_tid&&gate.holder_gate&&gate.requester_gate&&
                     gate.holder_gate!=gate.requester_gate)?1U:0U;
  smtw_append(&text,"gate %x h%u s%u t%llx q%u w%u a%u/%u r%u c%u d%u\n",
   (unsigned)(gate.holder_gate&0xffffffffU),gate.holder_tid,gate.holder_stage,
   (unsigned long long)(gate.holder_tid&&gate_now>gate.holder_since?gate_now-gate.holder_since:0),
   gate.requester_tid,gate.waits,gate.acquires,gate.releases,gate.last_holder_tid,conflict,gate.dropped);
 }
 if(s->main_count==1) {
  struct smtw_site *m=&s->sites[s->main_index];
  smtw_append(&text,"main t%llu st%x tag%x why%u pc%llx lr%llx fp%llx esr%llx far%llx\n"
   "wait%llx cont%llx kdepth%u kstop%u\n",
   m->tid,m->state,m->tag,m->why,m->pc,m->lr,m->fp,m->esr,m->far,
   m->wait,m->continuation,m->kcount,m->kstop);
  if(!m->why) {
   smtw_append(&text,"ipc17 x16=%llx\n",m->trap);
   for(unsigned a=0;a<8;a+=2)
    smtw_append(&text,"x%u=%llx x%u=%llx\n",a,m->args[a],a+1,m->args[a+1]);
  }
  for(unsigned i=0;i<m->kcount;++i)smtw_append(&text,"k%u %llx\n",i,m->kframes[i]);
 }
 unsigned workers=0;
 for(unsigned i=0;i<s->kept && workers<2;++i) {
  struct smtw_site *w=&s->sites[i];if(w->tag&THREAD_TAG_MAINTHREAD)continue;
  smtw_append(&text,"worker t%llu st%x why%u pc%llx lr%llx kstop%u\n",w->tid,w->state,w->why,w->pc,w->lr,w->kstop);++workers;
 }
 if(text.cut)memcpy(text.data+sizeof(text.data)-12,"\nwait-cut\n\0",11);
 smtw_publish(text.data);
}
static void smtw_worker(thread_call_param_t unused0,thread_call_param_t unused1) {
 (void)unused0;(void)unused1;++smtw_progress.polls;
 smtw_phase_step("entry",smtw_progress.polls);
 // Never allocate, iterate tasks, copy user pages or print in unsafe context.
 if(ml_at_interrupt_context() || get_preemption_level()!=0 || !ml_get_interrupts_enabled()) {
  smtw_progress.done=true;
 smtw_store(smtw_phase,SMTW_PHASE_TEXT,&smtw_phase_seq,"thermal-wait3 phaseUnsafe refused\n");
 return;
 }
 smtw_phase_step("select",smtw_progress.polls);
 struct smtw_selection selected={0};
 native_sm8250_thermal_wait_select(&selected);
 smtw_progress.last_scan=selected.scanned;smtw_progress.last_match=selected.matched;
 smtw_progress.last_pid=selected.pid;
 bool captured=selected.task!=TASK_NULL;
 if(captured) {
  smtw_phase_step("capture",smtw_progress.polls);
  bzero(&smtw_sample,sizeof(smtw_sample));
  smtw_sample.attempt=++smtw_progress.samples;
  smtw_sample.tick=mach_absolute_time();
  smtw_sample.scanned=selected.scanned;
  smtw_sample.pid=selected.pid;smtw_sample.uid=selected.uid;
  smtw_sample.uuid_source=selected.uuid_source;
  memcpy(smtw_sample.exec_uuid,selected.exec_uuid,sizeof(smtw_sample.exec_uuid));
  if(!smtw_sample.uuid_source)smtw_sample.identity_error=1;
  else if(memcmp(smtw_sample.exec_uuid,smtw_expected_uuid,16))smtw_sample.identity_error=9;
  smtw_capture(selected.task,&smtw_sample,false,0);
  task_deallocate(selected.task);
  smtw_phase_step("captured",smtw_progress.polls);
 } else ++smtw_progress.misses; // never erase the last referenced-task snapshot
 smtw_progress.done=smtw_progress.samples>=2 || smtw_progress.polls>=24;
 smtw_format(&smtw_sample);
 if(!smtw_progress.done) {
  uint64_t deadline;clock_interval_to_deadline(captured?60:15,NSEC_PER_SEC,&deadline);
  smtw_phase_step("enqueue",smtw_progress.polls);
  bool pending=thread_call_enter_delayed(smtw_call,deadline);
  if(!pending)++smtw_progress.rearm;
  char queued[SMTW_PHASE_TEXT];
  int count=smtw_format_into(queued,sizeof(queued),
   "thermal-wait3 queued poll%u at%llx deadline%llx pending%u rearm%u\n",
   smtw_progress.polls,(unsigned long long)mach_absolute_time(),
   (unsigned long long)deadline,pending?1U:0U,smtw_progress.rearm);
  if(count>0)smtw_store(smtw_phase,SMTW_PHASE_TEXT,&smtw_phase_seq,queued);
 } else smtw_phase_step("done",smtw_progress.polls);
}
static thread_call_t smui_call;
static struct smtw_sample smui_sample;
static unsigned smui_polls;
static unsigned smui_samples[3];
static const char *const smui_names[3]={"SpringBoard","backboardd","chronod"};

static void
smui_emit(const char *name, const struct smtw_sample *sample)
{
 char exec[33],cache[33];
 smtw_hex(exec,sample->exec_uuid);
 smtw_hex(cache,sample->cache_uuid);
 printf("GUIWAIT27 sample %s n%u pid%d task%llx tick%llx base%llx exec%s cache%s slide%x kept%u total%u main%u copy%u retire%u\n",
  name,sample->attempt,sample->pid,sample->taskid,sample->tick,sample->base,
  exec,cache,sample->slide,sample->kept,sample->total,sample->main_count,
  native_sm_gop_status(3),native_sm_gop_status(4));
 for(unsigned i=0;i<sample->kept;++i) {
  const struct smtw_site *site=&sample->sites[i];
  printf("GUIWAIT27 thread %s n%u t%llu tag%x st%x why%u pc%llx lr%llx wait%llx cont%llx trap%llx\n",
   name,sample->attempt,site->tid,site->tag,site->state,site->why,
   site->pc,site->lr,site->wait,site->continuation,site->trap);
  if(site->count) {
   struct smtw_text line={0};
   smtw_append(&line,"GUIWAIT27 frames %s n%u t%llu",name,sample->attempt,site->tid);
   for(unsigned j=0;j<site->count;++j)
    smtw_append(&line," %llx",site->uframes[j]);
   printf("%s\n",line.data);
  }
 }
 printf("GUIWAIT27 end %s n%u\n",name,sample->attempt);
}

/* A single matched receive per boot. This task reference belongs to the
 * callout and is released after the reply or after the third sample. */
static struct {
 unsigned slot, ended, samples;
 int pid;
 uint64_t tid;
 uint8_t uuid[16];
 task_t task;
 thread_call_t call;
} lsd_wait;
static struct smtw_sample lsd_sample;

static void
lsd_wait_worker(thread_call_param_t unused0,thread_call_param_t unused1)
{
 (void)unused0;(void)unused1;
 if(!__atomic_load_n(&lsd_wait.ended,__ATOMIC_ACQUIRE)) {
  bzero(&lsd_sample,sizeof(lsd_sample));
  lsd_sample.attempt=++lsd_wait.samples;
  lsd_sample.pid=lsd_wait.pid;
  lsd_sample.tick=mach_absolute_time();
  memcpy(lsd_sample.exec_uuid,lsd_wait.uuid,sizeof(lsd_sample.exec_uuid));
  smtw_capture(lsd_wait.task,&lsd_sample,true,true);
  if(!__atomic_load_n(&lsd_wait.ended,__ATOMIC_ACQUIRE)) {
   printf("LSDWAIT slot%u receiver_tid%llu sample%u kept%u identity%u\n",
    lsd_wait.slot,(unsigned long long)lsd_wait.tid,lsd_wait.samples,lsd_sample.kept,lsd_sample.identity_error);
   if(!lsd_sample.identity_error && lsd_sample.kept)smui_emit("lsd",&lsd_sample);
  }
  if(lsd_wait.samples<3 && !__atomic_load_n(&lsd_wait.ended,__ATOMIC_ACQUIRE)) {
   uint64_t deadline;
   clock_interval_to_deadline(5,NSEC_PER_SEC,&deadline);
   thread_call_enter_delayed(lsd_wait.call,deadline);
   return;
  }
 }
 task_deallocate(lsd_wait.task);
 thread_call_free(lsd_wait.call);
}

void native_sm8250_lsd_wait_begin(unsigned slot,int pid,const unsigned char *uuid);
void native_sm8250_lsd_wait_end(unsigned slot);

void
native_sm8250_lsd_wait_begin(unsigned slot,int pid,const unsigned char *uuid)
{
 unsigned empty=0;
 if(!__atomic_compare_exchange_n(&lsd_wait.slot,&empty,slot,false,__ATOMIC_RELEASE,__ATOMIC_RELAXED))return;
 lsd_wait.call=thread_call_allocate_with_priority(lsd_wait_worker,NULL,THREAD_CALL_PRIORITY_LOW);
 if(!lsd_wait.call) {
  printf("LSDWAIT slot%u allocation failed\n",slot);
  return;
 }
 lsd_wait.task=current_task();
 task_reference(lsd_wait.task);
 lsd_wait.tid=thread_tid(current_thread());
 lsd_wait.pid=pid;
 memcpy(lsd_wait.uuid,uuid,sizeof(lsd_wait.uuid));
 uint64_t deadline;
 clock_interval_to_deadline(2,NSEC_PER_SEC,&deadline);
 thread_call_enter_delayed(lsd_wait.call,deadline);
}

void
native_sm8250_lsd_wait_end(unsigned slot)
{
 if(slot==__atomic_load_n(&lsd_wait.slot,__ATOMIC_ACQUIRE))
  __atomic_store_n(&lsd_wait.ended,1U,__ATOMIC_RELEASE);
}

static void
smui_worker(thread_call_param_t unused0,thread_call_param_t unused1)
{
 (void)unused0;(void)unused1;
 if(ml_at_interrupt_context() || get_preemption_level()!=0 || !ml_get_interrupts_enabled())return;
 ++smui_polls;
 for(unsigned i=0;i<3;++i) {
  if(smui_samples[i]>=(i==1?2:4))continue;
  struct smtw_selection selected={0};
  native_sm8250_gui_wait_select(&selected,smui_names[i],smui_exec_uuid[i]);
  if(!selected.task) {
   if(i==2)printf("GUIWAIT27 chronod scan%u match%u uuid%u pid%d poll%u\n",
    selected.scanned,selected.matched,selected.uuid_source,selected.pid,smui_polls);
   continue;
  }
  bzero(&smui_sample,sizeof(smui_sample));
  smui_sample.pid=selected.pid;
  smui_sample.tick=mach_absolute_time();
  memcpy(smui_sample.exec_uuid,selected.exec_uuid,16);
  smtw_capture(selected.task,&smui_sample,true,0);
  task_deallocate(selected.task);
  if(smui_sample.identity_error || !smui_sample.base || (i!=2 && smui_sample.main_count!=1) ||
     memcmp(smui_sample.cache_uuid,smui_cache_uuid,16))continue;
  smui_sample.attempt=++smui_samples[i];
  smui_emit(smui_names[i],&smui_sample);
 }
 if(smui_polls>=24 || (smui_samples[0]>=4 && smui_samples[1]>=2 && smui_samples[2]>=4)) {
  printf("GUIWAIT27 done polls%u sb%u bb%u ch%u\n",smui_polls,smui_samples[0],smui_samples[1],smui_samples[2]);
  return;
 }
 uint64_t deadline;
 clock_interval_to_deadline(30,NSEC_PER_SEC,&deadline);
 thread_call_enter_delayed(smui_call,deadline);
}

static void
smui_prepare(void)
{
 unsigned enable=0;
 if(!PE_parse_boot_argn("gui-wait-diag",&enable,sizeof(enable)) || enable!=1)return;
 smui_call=thread_call_allocate_with_priority(smui_worker,NULL,THREAD_CALL_PRIORITY_LOW);
 if(!smui_call)return;
 uint64_t deadline;
 clock_interval_to_deadline(150,NSEC_PER_SEC,&deadline);
 thread_call_enter_delayed(smui_call,deadline);
 printf("GUIWAIT27 armed\n");
}


int native_sm8250_thermal_wait_prepare(void) {
 if(__atomic_exchange_n(&smtw_started,1U,__ATOMIC_ACQ_REL))return KERN_FAILURE;
 smtw_phase_step("armed",0);
 smtw_publish("thermal-wait3 pending; initial30s retry15s cap24 samples2\n");
 smtw_call=thread_call_allocate_with_priority(smtw_worker,NULL,THREAD_CALL_PRIORITY_LOW);
 if(!smtw_call) {smtw_phase_step("alloc-failed",0);return KERN_RESOURCE_SHORTAGE;}
 uint64_t deadline;clock_interval_to_deadline(30,NSEC_PER_SEC,&deadline);
 thread_call_enter_delayed(smtw_call,deadline);smui_prepare();return 0;
}
