// SPDX-License-Identifier: MIT
// Two board-local read-only TSENS snapshots for thermal bring-up. No IOService
// identity or Apple sensor keys are published until measurements are qualified.
#include "SmTsensReadOnly.h"
#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <kern/thread_call.h>
#include <machine/machine_routines.h>
#include <mach/vm_prot.h>
#include <sys/errno.h>
#include <stdarg.h>
// Exact current XNU arm/machine_routines.h interface, otherwise hidden from
// this IOKit compile record by its MACH/PEXPERT_KERNEL_PRIVATE guard.
extern "C" vm_offset_t ml_io_map_with_prot(vm_offset_t,vm_size_t,vm_prot_t);
namespace {
struct Observation {
 unsigned phase,error,mapped,samples;
 uint64_t first_time,last_time;
 SmTsensGroup first[2],last[2];
};
static Observation observation;
static vm_offset_t mappings[2];
static thread_call_t second_sample;
static unsigned published;
// Two immutable published buffers: a panic reader can retain the first buffer
// while the sole delayed callback constructs the second without a data race.
static char notes[2][1024];
static uint32_t read_status(void *opaque,unsigned offset) {
 // Both callers use only a0..dc and e4, within the pinned 0x1ff TM ranges.
 vm_offset_t base=reinterpret_cast<vm_offset_t>(opaque);
 __asm__ volatile("dmb oshld" ::: "memory");
 uint32_t word=*reinterpret_cast<volatile const uint32_t *>(base+offset);
 __asm__ volatile("dmb oshld" ::: "memory");
 return word;
}
static void append(char *buffer,size_t capacity,size_t &at,const char *format,...) {
 if(at>=capacity)return;
 va_list args;va_start(args,format);int n=vsnprintf(buffer+at,capacity-at,format,args);va_end(args);
 if(n>0)at+=static_cast<size_t>(n)<capacity-at?static_cast<size_t>(n):capacity-at-1;
}
static void format_note(unsigned index) {
 char *note=notes[index];const size_t capacity=sizeof(notes[index]);
 size_t at=0;
 append(note,capacity,at,"thermal-h4a phase%u errno%u map%x samples%u ro1 writes0 time%llu/%llu\n",
  observation.phase,observation.error,observation.mapped,observation.samples,
  static_cast<unsigned long long>(observation.first_time),static_cast<unsigned long long>(observation.last_time));
 for(unsigned g=0;g<2;++g) {
  const auto &a=observation.first[g];const auto &b=observation.last[g];
  append(note,capacity,at,"tsens%u first%x/%x v%x last%x/%x v%x raw",g,a.ready_before,a.ready_after,a.valid,b.ready_before,b.ready_after,b.valid);
  for(unsigned s=0;s<16;++s)append(note,capacity,at," %x",b.raw[s]);
  append(note,capacity,at,"\n");
 }
}
static void acquire(thread_call_param_t,thread_call_param_t) {
 // Only one delayed callback exists; launch-time capture finishes before queueing.
 SmTsensGroup groups[2];
 for(unsigned g=0;g<2;++g)sm_tsens_capture(read_status,reinterpret_cast<void *>(mappings[g]),&groups[g]);
 uint64_t now=mach_absolute_time();
 if(!observation.samples) {
  observation.first_time=now;
  for(unsigned g=0;g<2;++g)observation.first[g]=groups[g];
 }
 observation.last_time=now;
 for(unsigned g=0;g<2;++g)observation.last[g]=groups[g];
 ++observation.samples;observation.phase=observation.samples==2?3U:2U;
 unsigned index=observation.samples-1;format_note(index);
 __atomic_store_n(&published,index+1,__ATOMIC_RELEASE);
 IOLog("%s",notes[index]);
}
}
extern "C" const char *native_sm8250_thermal_note(void) {
 unsigned index=__atomic_load_n(&published,__ATOMIC_ACQUIRE);
 return index?notes[index-1]:"thermal-h4a not prepared\n";
}
extern "C" int native_sm8250_thermal_prepare(void) {
 if(observation.phase)return EALREADY;
 observation.phase=1;
 // Original device DTB SHA74b114... and target kernel DTS agree on these ranges.
 constexpr vm_offset_t physical[2]={0x0c263000,0x0c265000};
 for(unsigned g=0;g<2;++g) {
  mappings[g]=ml_io_map_with_prot(physical[g],0x1ff,VM_PROT_READ);
  if(!mappings[g]) {observation.error=ENOMEM;break;}
  observation.mapped|=1U<<g;
 }
 if(!observation.error) {
  second_sample=thread_call_allocate(acquire,nullptr);
  if(!second_sample)observation.error=ENOMEM;
 }
 if(observation.error) {
  // The IO map API used here creates boot-lifetime mappings; preserve any
  // acquired mappings without exposing or accessing them after prepare fails.
  format_note(0);__atomic_store_n(&published,1U,__ATOMIC_RELEASE);return observation.error;
 }
 acquire(nullptr,nullptr);
 uint64_t deadline;clock_interval_to_deadline(10,NSEC_PER_SEC,&deadline);
 thread_call_enter_delayed(second_sample,deadline);
 // Both mappings and the single one-shot thread_call live until this boot ends.
 return 0;
}
