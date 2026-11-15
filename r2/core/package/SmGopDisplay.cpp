// SPDX-License-Identifier: BSD-2-Clause-Patent
// Board wiring for UniXNU LegacyDisplay and the optional GPU GOP presenter.
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOUserClient.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSSymbol.h>
#include <pexpert/pexpert.h>
#include <pexpert/arm64/boot.h>
#include <mach/kmod.h>
#include <mach/mach_types.h>
#include <mach/processor_info.h>
#include <kern/clock.h>
#include <kern/thread_call.h>
#include <kern/queue.h>
#include <kern/priority_queue.h>
extern "C" {
#include <kern/timer_call.h>
}
#include <machine/machine_routines.h>
#include "Sm8250GpuReadOnly.h"
#include "Sm8250GpuCx.h"
#include "Sm8250AliothGmu.h"
#include "In2020BacklightTransport.h"
#include "In2020BacklightService.h"
extern "C" void sm_gop_board_static_init(void) __asm__("__GLOBAL__sub_I_SmGopDisplay.cpp");
extern "C" {
extern volatile uint32_t armcpu_core_timer[8], armcpu_core_sgi[8];
extern volatile uint32_t armcpu_smp_requested, armcpu_smp_running, armcpu_smp_step[8];
extern volatile uint32_t armcpu_smp_mpidr[8], armcpu_smp_seen_mpidr[8], armcpu_smp_fail[8];
extern volatile uint32_t armcpu_smp_conduit;
extern volatile uint32_t armcpu_boot_mpidr;
extern volatile int32_t armcpu_smp_psci_rc[8];
// Exact cpu_xcall signature in the selected XNU.
kern_return_t cpu_xcall(int cpu,void (*callback)(void *),void *context);
// Exact prototypes from the selected XNU's processor.h and sched_prim.h.
processor_t cpu_to_processor(int cpu);
void processor_cpu_load_info(processor_t processor,natural_t ticks[]);
unsigned native_sm_gop_status(unsigned field);
extern volatile uint32_t native_touch_hid_reports;
int native_sm8250_thermal_live_copy(unsigned,unsigned,int32_t *,uint64_t *);
processor_t thread_bind(processor_t processor);
wait_result_t thread_block(thread_continue_t continuation);
}

namespace {
uint64_t cpuControlStart;
uint32_t cpuControlTimerStart;
constexpr int kSampleCpuSlots=8;
uint32_t cpuControlTimer[kSampleCpuSlots], cpuControlSgi[kSampleCpuSlots];
thread_call_t cpuControlCall;

// The callback may arrive after the callout's sample window. Keep its storage
// for the lifetime of the kernel, and publish the actual MPIDR with completion.
uint64_t cpuIpiReply[kSampleCpuSlots][kSampleCpuSlots];
timer_call_data_t cpuTimerProbe[kSampleCpuSlots];
uint64_t cpuTimerReply[kSampleCpuSlots];

void recordIpiReply(void *context)
{
 uint64_t mpidr;
 __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
 __atomic_store_n(static_cast<uint64_t *>(context),mpidr,__ATOMIC_RELEASE);
}

void testRuntimeIpi(int source,uint32_t sourceMpidr,int target)
{
 const processor_t sender=cpu_to_processor(source);
 const processor_t boot=cpu_to_processor(0);
 if(!sender || !boot) {
  printf("ARMCPU-P2I source=%d target=%d skipped=no-processor\n",source,target);
  return;
 }
 const processor_t previous=thread_bind(sender);
 thread_block(nullptr);
 uint64_t actual;
 __asm__ volatile("mrs %0, mpidr_el1" : "=r"(actual));
 const bool matched=(actual&0xff00ffffffULL)==(sourceMpidr&0x00ffffffU);
 const kern_return_t rc=matched?cpu_xcall(target,recordIpiReply,&cpuIpiReply[source][target]):KERN_FAILURE;
 if(rc==KERN_SUCCESS) {
  thread_bind(boot);
  thread_block(nullptr);
  IOSleep(100);
 }
 thread_bind(previous);
 const uint64_t reply=__atomic_load_n(&cpuIpiReply[source][target],__ATOMIC_ACQUIRE);
 printf("ARMCPU-P2I source=%d mpidr=%llx target=%d matched=%u xcall_rc=%d reply_mpidr=%llx\n",
        source,static_cast<unsigned long long>(actual),target,matched?1U:0U,rc,
        static_cast<unsigned long long>(reply));
}

void recordTimerReply(timer_call_param_t context,timer_call_param_t)
{
 recordIpiReply(context);
}

void testLocalTimer(int cpu,uint32_t expectedMpidr)
{
 const processor_t target=cpu_to_processor(cpu);
 const processor_t boot=cpu_to_processor(0);
 if(!target || !boot) {
  printf("ARMCPU-P4T cpu=%d skipped=no-processor\n",cpu);
  return;
 }

 timer_call_t probe=&cpuTimerProbe[cpu];
 timer_call_setup(probe,recordTimerReply,&cpuTimerReply[cpu]);
 const processor_t previous=thread_bind(target);
 thread_block(nullptr);
 uint64_t source;
 __asm__ volatile("mrs %0, mpidr_el1" : "=r"(source));
 const bool matched=(source&0xff00ffffffULL)==(expectedMpidr&0x00ffffffU);
 const uint32_t before=__atomic_load_n(&armcpu_core_timer[cpu],__ATOMIC_RELAXED);
 if(matched) {
  uint64_t deadline;
  clock_interval_to_deadline(20,NSEC_PER_MSEC,&deadline);
  timer_call_enter(probe,deadline,TIMER_CALL_SYS_CRITICAL|TIMER_CALL_LOCAL);
  // Wait on CPU0 so a missing interrupt on the target cannot strand this test.
  thread_bind(boot);
  thread_block(nullptr);
  IOSleep(100);
  timer_call_cancel(probe);
 }
 thread_bind(previous);
 const uint64_t reply=__atomic_load_n(&cpuTimerReply[cpu],__ATOMIC_ACQUIRE);
 const uint32_t after=__atomic_load_n(&armcpu_core_timer[cpu],__ATOMIC_RELAXED);
 printf("ARMCPU-P4T cpu=%d armed=%u source_mpidr=%llx reply_mpidr=%llx ppi_delta=%u\n",
        cpu,matched?1U:0U,static_cast<unsigned long long>(source),
        static_cast<unsigned long long>(reply),after-before);
}

void cpuControlSample(thread_call_param_t, thread_call_param_t)
{
 const uint64_t now=mach_absolute_time();
 const uint32_t timer=__atomic_load_n(&armcpu_core_timer[0],__ATOMIC_RELAXED);
 uint64_t elapsedNs=0;
 absolutetime_to_nanoseconds(now-cpuControlStart,&elapsedNs);
 unsigned started=0;
 for(unsigned cpu=1;cpu<8;++cpu)
  if(__atomic_load_n(&armcpu_smp_step[cpu],__ATOMIC_RELAXED))++started;
 printf("ARMCPU-P1 t0=%llu t1=%llu elapsed_ns=%llu core0=%u core0b=%u count=%u\n"
        "ARMCPU-P1 smpreq=%u running=%u started=%u/7 sec1_step=%u\n",
        cpuControlStart,now,elapsedNs,cpuControlTimerStart,timer,timer-cpuControlTimerStart,
        __atomic_load_n(&armcpu_smp_requested,__ATOMIC_RELAXED),
        __atomic_load_n(&armcpu_smp_running,__ATOMIC_RELAXED),started,
        __atomic_load_n(&armcpu_smp_step[1],__ATOMIC_RELAXED));
 if(__atomic_load_n(&armcpu_smp_requested,__ATOMIC_RELAXED)) {
  uint64_t cpumask=0;
  PE_parse_boot_argn("cpumask",&cpumask,sizeof(cpumask));
  unsigned redistIpi=0;
  PE_parse_boot_argn("armcpu-redist-ipi",&redistIpi,sizeof(redistIpi));
  int slots=2;
  if(cpumask==0x13)
   slots=3;
  else if(cpumask==0x93)
   slots=4;
  else if(cpumask==0xd3)
   slots=5;
  else if(cpumask==0xf3)
   slots=6;
  else if(cpumask==0xf7)
   slots=7;
  else if(cpumask==0xff)
   slots=kSampleCpuSlots;
  for(int cpu=1;cpu<slots;++cpu) {
   const unsigned step=__atomic_load_n(&armcpu_smp_step[cpu],__ATOMIC_RELAXED);
   const uint32_t want=__atomic_load_n(&armcpu_smp_mpidr[cpu],__ATOMIC_RELAXED);
   const uint32_t seen=__atomic_load_n(&armcpu_smp_seen_mpidr[cpu],__ATOMIC_RELAXED);
   const uint32_t fail=__atomic_load_n(&armcpu_smp_fail[cpu],__ATOMIC_RELAXED);
   const int rc=__atomic_load_n(&armcpu_smp_psci_rc[cpu],__ATOMIC_RELAXED);
   const uint32_t timer=__atomic_load_n(&armcpu_core_timer[cpu],__ATOMIC_RELAXED);
   const uint32_t sgi=__atomic_load_n(&armcpu_core_sgi[cpu],__ATOMIC_RELAXED);
   const char *psci=step<4?"before-call":rc?"nonzero-return":step>=5?"entered":"pending-or-zero";
   printf("ARMCPU-P3 cpu=%d step=%u psci=%s rc_raw=%d fail=%u want=%x seen=%x timer_delta=%u sgi_delta=%u\n",
          cpu,step,psci,rc,fail,want,seen,timer-cpuControlTimer[cpu],sgi-cpuControlSgi[cpu]);
   if(step==7 && !fail && want!=0 && (seen&0xffff)==want) {
    if(redistIpi==1) {
     testRuntimeIpi(0,__atomic_load_n(&armcpu_boot_mpidr,__ATOMIC_RELAXED),cpu);
     testRuntimeIpi(cpu,seen,0);
    }
    if(timer==cpuControlTimer[cpu])
     testLocalTimer(cpu,seen);
   }
  }
 }
 thread_call_free(cpuControlCall);
 cpuControlCall=nullptr;
}

// Alioth CpuFreqProbe read-only path. IN2020 factory EPSS windows cover
// CPU0-3, CPU4-6 and CPU7. Keep domain state separate from the request LUT.
constexpr uint64_t kEpssBase[] = {0x18591000,0x18592000,0x18593000};
constexpr int kEpssCpuBoundary[] = {0,4,7,8};
constexpr uint32_t kEpssEnable=0x0,kEpssState=0x20,kEpssLut=0x100,kEpssRequest=0x320;
constexpr uint32_t kEpssLutEntries=40;
IOMemoryMap *epssMap[3];
uint32_t epssTicks[8][CPU_STATE_MAX];
thread_call_t epssCall;
unsigned epssWaiting,epssSamples;
bool epssVoteEnabled;
constexpr unsigned kEpssVoteEarliest=5,kEpssVoteSamples=6,kEpssVoteMinReports=40;
unsigned epssVoteStart,epssPreviousTouches;
bool epssVoteCancelled;
constexpr uint32_t kEpssDcvsCtrl=0xb0,kEpssVoteCapKHz=1400000;
struct EpssVote {
 uint32_t level,saved[3];
 unsigned slots,changed;
};
EpssVote epssVotes[3];

uint32_t epssRead(uintptr_t base,uint32_t offset)
{
 return *reinterpret_cast<const volatile uint32_t *>(base+offset);
}

// Alioth and qcom-cpufreq-hw vote through PERF_STATE only. On IN2020
// the factory qcom,freq-domain cells specify max_cores=4 for both P domains.
void epssWriteRequest(uintptr_t va,unsigned slot,uint32_t level)
{
 *reinterpret_cast<volatile uint32_t *>(va+kEpssRequest+slot*4)=level;
 __asm__ volatile("dsb sy" ::: "memory");
}

bool epssCool()
{
 // Factory tsens phandle 0x18 is group 0, Gold channels 7 through 14.
 for(unsigned channel=7;channel<=14;++channel) {
  int32_t millicelsius=0;
  uint64_t timestamp=0;
  if(native_sm8250_thermal_live_copy(0,channel,&millicelsius,&timestamp) ||
     millicelsius<0 || millicelsius>=65000)return false;
 }
 return true;
}

void epssRestore(unsigned d,uintptr_t va)
{
 EpssVote &vote=epssVotes[d];
 for(unsigned slot=0;slot<vote.slots;++slot) {
  if(!(vote.changed&(1U<<slot)))continue;
  const uint32_t current=epssRead(va,kEpssRequest+slot*4);
  if(current==vote.level)epssWriteRequest(va,slot,vote.saved[slot]);
  const uint32_t readback=epssRead(va,kEpssRequest+slot*4);
  printf("EPSS-VOTE restore d=%u slot=%u prev=%u target=%u readback=%u\n",
         d,slot,current,vote.saved[slot],readback);
  if(readback!=vote.level)vote.changed&=~(1U<<slot);
 }
}

void epssVote(unsigned d,uintptr_t va)
{
 EpssVote &vote=epssVotes[d];
 uint32_t previous=0,best=0,rows=0;
 for(unsigned i=0;i<kEpssLutEntries;++i) {
  const uint32_t row=epssRead(va,kEpssLut+i*4);
  const uint32_t frequency=row>>30?(row&0xff)*19200:0;
  if(i && frequency==previous)break;
  previous=frequency;
  rows=i+1;
  if(((row>>16)&7)==4 && frequency>best && frequency<=kEpssVoteCapKHz) {
   best=frequency;
   vote.level=i;
  }
 }
 if(!best) {printf("EPSS-VOTE skip d=%u no eligible LUT entry\n",d);return;}
 vote.slots=(epssRead(va,kEpssDcvsCtrl)&1)?kEpssCpuBoundary[d+1]-kEpssCpuBoundary[d]:1;
 for(unsigned slot=0;slot<vote.slots;++slot) {
  vote.saved[slot]=epssRead(va,kEpssRequest+slot*4);
  if(vote.saved[slot]>=rows) {
   printf("EPSS-VOTE skip d=%u slot=%u invalid request=%u\n",d,slot,vote.saved[slot]);
   return;
  }
 }
 for(unsigned slot=0;slot<vote.slots;++slot) {
  const uint32_t previousRow=epssRead(va,kEpssLut+vote.saved[slot]*4);
  if(!(previousRow>>30) || (previousRow&0xff)*19200>=best)continue;
  epssWriteRequest(va,slot,vote.level);
  const uint32_t readback=epssRead(va,kEpssRequest+slot*4);
  printf("EPSS-VOTE apply d=%u slot=%u old=%u new=%u khz=%u readback=%u\n",
         d,slot,vote.saved[slot],vote.level,best,readback);
  if(readback!=vote.level) {epssRestore(d,va);return;}
  vote.changed|=1U<<slot;
 }
}

void epssSchedule(unsigned seconds)
{
 uint64_t deadline;
 clock_interval_to_deadline(seconds,NSEC_PER_SEC,&deadline);
 thread_call_enter_delayed(epssCall,deadline);
}

void epssSample(thread_call_param_t,thread_call_param_t)
{
 const unsigned frames=native_sm_gop_status(4);
 const unsigned touches=__atomic_load_n(&native_touch_hid_reports,__ATOMIC_RELAXED);
 if(!epssSamples && (!frames || !touches || __atomic_load_n(&armcpu_smp_running,__ATOMIC_ACQUIRE)!=7)) {
  if(++epssWaiting<240) {epssSchedule(5);return;}
  printf("EPSS-P5 unavailable: no frame and touch with eight CPUs online\n");
 } else {
  uint64_t ns=0;
  absolutetime_to_nanoseconds(mach_absolute_time(),&ns);
  printf("EPSS-P5 sample=%u boot_ms=%llu frames=%u touch=%u\n",epssSamples,ns/1000000,frames,touches);
  const unsigned touchDelta=touches-epssPreviousTouches;
  epssPreviousTouches=touches;
  if(epssVoteEnabled && !epssVoteStart && epssSamples>=kEpssVoteEarliest &&
     touchDelta>=kEpssVoteMinReports) {
   epssVoteStart=epssSamples;
   printf("EPSS-VOTE trigger sample=%u touch_delta=%u\n",epssSamples,touchDelta);
  }
  const bool voteWindow=epssVoteStart && epssSamples<epssVoteStart+kEpssVoteSamples;
  if(voteWindow && !epssVoteCancelled && !epssCool()) {
   epssVoteCancelled=true;
   printf("EPSS-VOTE thermal gate sample=%u\n",epssSamples);
  }
  const bool cool=voteWindow && !epssVoteCancelled;
  for(unsigned d=0;d<3;++d) {
   if(!epssSamples) {
    IODeviceMemory *memory=IODeviceMemory::withRange(kEpssBase[d],0x1000);
    if(memory) {
     const IOOptionBits access=(epssVoteEnabled && d!=0)?0:kIOMapReadOnly;
     epssMap[d]=memory->map(kIOMapAnywhere|kIOMapInhibitCache|access);
     memory->release();
    }
   }
   uint32_t busy=0,idle=0;
   for(int cpu=kEpssCpuBoundary[d];cpu<kEpssCpuBoundary[d+1];++cpu) {
    processor_t processor=cpu_to_processor(cpu);
    if(!processor)continue;
    natural_t ticks[CPU_STATE_MAX]={};
    processor_cpu_load_info(processor,ticks);
    if(epssSamples) {
     busy+=(ticks[CPU_STATE_USER]-epssTicks[cpu][CPU_STATE_USER])+
           (ticks[CPU_STATE_SYSTEM]-epssTicks[cpu][CPU_STATE_SYSTEM]);
     idle+=ticks[CPU_STATE_IDLE]-epssTicks[cpu][CPU_STATE_IDLE];
    }
    memcpy(epssTicks[cpu],ticks,sizeof(ticks));
   }
   if(!epssMap[d]) {
    printf("EPSS-P5 d=%u nomap busy=%u idle=%u\n",d,busy,idle);
    continue;
   }
   const uintptr_t va=epssMap[d]->getVirtualAddress();
   const uint32_t enabled=epssRead(va,kEpssEnable)&1;
   if(epssVoteEnabled && d!=0) {
    if(epssVotes[d].changed && !cool && enabled)epssRestore(d,va);
    if(epssSamples==epssVoteStart && cool && enabled)epssVote(d,va);
   }
   const uint32_t state=epssRead(va,kEpssState);
   const uint32_t request=epssRead(va,kEpssRequest);
   const uint32_t row=request<kEpssLutEntries?epssRead(va,kEpssLut+4*request):0;
   printf("EPSS-P5 d=%u en=%u state=%08x state_xo_khz=%u req0=%u lut=%08x busy=%u idle=%u\n",
          d,enabled,state,(state&0xff)*19200,request,row,busy,idle);
  }
  ++epssSamples;
  if(epssSamples<37 || (epssVoteStart && epssSamples<=epssVoteStart+kEpssVoteSamples)) {
   epssSchedule(5);return;
  }
 }
 if(epssVoteEnabled && !epssVoteStart)printf("EPSS-VOTE no qualifying touch window\n");
 for(unsigned d=0;d<3;++d) {
  if(epssVotes[d].changed)
   printf("EPSS-VOTE unrestored d=%u mask=%x\n",d,epssVotes[d].changed);
  if(epssMap[d])epssMap[d]->release();
 }
 thread_call_free(epssCall);
 printf("EPSS-P5 done samples=%u\n",epssSamples);
}

void startEpssSample()
{
 unsigned enabled=0;
 uint64_t mask=0;
 if(!PE_parse_boot_argn("sm8250-ep-read",&enabled,sizeof(enabled)) || enabled!=1 ||
    !PE_parse_boot_argn("cpumask",&mask,sizeof(mask)) || mask!=0xff)return;
 unsigned vote=0;
 epssVoteEnabled=PE_parse_boot_argn("sm8250-ep-vote",&vote,sizeof(vote)) && vote==1;
 epssCall=thread_call_allocate_with_priority(epssSample,nullptr,THREAD_CALL_PRIORITY_LOW);
 if(epssCall)epssSchedule(5);
}

void startCpuControlSample()
{
 const auto *args=static_cast<const boot_args *>(PE_state.bootArgs);
 PE_Video video={};
 if(args && PE_current_console(&video)==0)
  printf("PLATFORM-MEM phys=%llx bytes=%llx boot_actual=%llx used_top=%llx fb=%llx stride=%lu height=%lu\n",
         args->physBase,args->memSize,args->memSizeActual,args->topOfKernelData,
         static_cast<unsigned long long>(video.v_baseAddr),
         static_cast<unsigned long>(video.v_rowBytes),static_cast<unsigned long>(video.v_height));
 cpuControlCall=thread_call_allocate(cpuControlSample,nullptr);
 if(!cpuControlCall) {
  printf("ARMCPU-P1 sample unavailable: callout allocation failed\n");
  return;
 }
 cpuControlStart=mach_absolute_time();
 cpuControlTimerStart=__atomic_load_n(&armcpu_core_timer[0],__ATOMIC_RELAXED);
 for(int cpu=1;cpu<kSampleCpuSlots;++cpu) {
  cpuControlTimer[cpu]=__atomic_load_n(&armcpu_core_timer[cpu],__ATOMIC_RELAXED);
  cpuControlSgi[cpu]=__atomic_load_n(&armcpu_core_sgi[cpu],__ATOMIC_RELAXED);
 }
 uint64_t deadline;
 clock_interval_to_deadline(4,NSEC_PER_SEC,&deadline);
 thread_call_enter_delayed(cpuControlCall,deadline);
}
}

extern "C" {
extern kmod_info_t g_kernel_kmod_info;
void native_keyevents_late_probe(void);
void sm_gop_static_init(void) __asm__("__GLOBAL__sub_I_NativeLegacyDisplay.cpp");
IOService *native_legacy_display_allocate(void);
void sm_gop_presenter_static_init(void) __asm__("__GLOBAL__sub_I_NativeGOPPresenter.cpp");
IOService *native_gop_presenter_create(void);
void native_gop_presenter_set_source(IOService *,IOService *);
bool native_gop_gpu_offer(IOService *,void *,IOMemoryDescriptor *,uint64_t,uint32_t,uint32_t);
static IOService *gpuHandoffPresenter;
int OSKextLoadKextWithIdentifier(const char *);
static IOService *activeDisplay;static IOPlatformDevice *activeNub;
static unsigned started,stage,validations,copies,retirements;static uint64_t bytesCopied;
unsigned native_sm_gop_status(unsigned field) {
 return field==0?stage:field==1?(activeDisplay?1U:0U):field==2?(activeNub?1U:0U):field==3?__atomic_load_n(&copies,__ATOMIC_RELAXED):__atomic_load_n(&retirements,__ATOMIC_RELAXED);
}
bool native_legacy_gpu_offer(void *display,IOMemoryDescriptor *memory,uint64_t stride,uint32_t width,uint32_t height) {
 IOService *presenter=__atomic_load_n(&gpuHandoffPresenter,__ATOMIC_ACQUIRE);
 return presenter && native_gop_gpu_offer(presenter,display,memory,stride,width,height);
}
void native_legacy_gpu_fallback_observed(int){}
void native_legacy_swap_stage(unsigned){}
void native_legacy_client_enter(task_t,uint32_t){}
void native_legacy_client_observed(IOReturn status,IOUserClient *) {
 printf("SMGOP20 client %x\n",status);
}
void native_legacy_pm_observed(unsigned event,IOReturn status,unsigned long state) {
 printf("SMGOP20 pm %u %x %lu\n",event,status,state);
}
void native_legacy_vsync_observed(unsigned,unsigned){}
void native_legacy_copy_timing_observed(uint64_t,uint64_t){}
void native_legacy_frame_observed(unsigned,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t){}
void native_legacy_frame_colors_observed(const uint32_t *,const uint64_t *,uint64_t){}
void native_legacy_surface_rejected(uint64_t w,uint64_t h,uint32_t f,uint32_t p,uint64_t s,uint64_t a,uint64_t n) {
 static unsigned count;if(count++<8)printf("SMGOP20 reject %llu/%llu fmt%x planes%u stride%llu alloc%llu mem%llu\n",w,h,f,p,s,a,n);
}
void native_legacy_swap_validation_observed(bool ok,unsigned why,const void *,const void *) {
 ++validations;if(validations<=8)printf("SMGOP20 validate %u why%u\n",ok?1U:0U,why);
}
void native_legacy_transfer_observed(IOReturn status,const void *,uint64_t bytes) {
 __atomic_add_fetch(&copies,1U,__ATOMIC_RELAXED);if(!status)bytesCopied+=bytes;
 if(copies<=8)printf("SMGOP20 copy %u status%x bytes%llu total%llu\n",copies,status,bytes,bytesCopied);
}
void native_legacy_retirement_observed(IOService *) {
 __atomic_add_fetch(&retirements,1U,__ATOMIC_RELAXED);if(retirements<=8)printf("SMGOP20 retire %u\n",retirements);
}
void native_legacy_registration_observed(IOService *s) {
 printf("SMGOP20 registered %s\n",s->getMetaClass()->getClassName());
}
bool native_legacy_prepare_scanout(void) {
 // Native XNU console handoff, not a panel power or DPU register operation.
 static bool owned;if(!owned){PE_initialize_console(nullptr,kPEDisableScreen);owned=true;}return true;
}
bool native_legacy_gop_mapping_observed(IOService *,IOMemoryMap *m,uint32_t w,uint32_t h) {
 PE_Video v={};if(PE_current_console(&v))return false;
 bool ok=v.v_baseAddr==m->getPhysicalAddress() && v.v_width==w && v.v_height==h &&
  v.v_depth==32 && !v.v_rotate && v.v_rowBytes>=uint64_t(w)*4 &&
  v.v_rowBytes<=UINT64_MAX/h && m->getLength()==v.v_rowBytes*h;
 printf("SMGOP20 map ok%u pa%llx bytes%llu %ux%u stride%llu\n",ok?1U:0U,m->getPhysicalAddress(),m->getLength(),w,h,(uint64_t)v.v_rowBytes);
 return ok;
}
__attribute__((noreturn)) void native_legacy_backend_guard(unsigned slot,void *,void *caller) {
 panic("SMGOP20 unsupported slot%u caller%p stage%u validate%u copy%u retire%u",slot,caller,stage,validations,copies,retirements);
}
void native_legacy_display_probe(void) {} // initialization is late, after real IOBSD publication
void native_legacy_display_late_probe(void) {
 if(started++)return;
 native_keyevents_late_probe();
 unsigned enable=0;PE_parse_boot_argn("smgop",&enable,sizeof(enable));if(enable!=1)return;
 gpuProbeReadOnly();
 gpuProbeCx();
 gpuProbeAliothGmu();
 stage=1;
 int family=OSKextLoadKextWithIdentifier("com.apple.iokit.IOMobileGraphicsFamily");
 int surface=OSKextLoadKextWithIdentifier("com.apple.iokit.IOSurface");
 const OSSymbol *name=OSSymbol::withCStringNoCopy("IOMobileFramebufferLegacy");
 const OSMetaClass *base=name?OSMetaClass::getMetaClassWithName(name):nullptr;
 OSSafeReleaseNULL(name);
 printf("SMGOP20 load family%d surface%d base%p\n",family,surface,base);
 if(family || surface || !base || base->getClassSize()!=0x18a0)return;
 OSDictionary *match=IOService::nameMatching("IOCoreSurfaceRoot");
 IOService *root=match?IOService::waitForMatchingService(match,5000000000ULL):nullptr;
 OSSafeReleaseNULL(match);bool haveSurface=root;OSSafeReleaseNULL(root);if(!haveSurface)return;
 void *handle=OSMetaClass::preModLoad(g_kernel_kmod_info.name);if(!handle)return;
 unsigned gpu=0;PE_parse_boot_argn("sheng-fb-gpu",&gpu,sizeof(gpu));
 sm_gop_board_static_init();
 sm_gop_static_init();
 if(gpu==1)sm_gop_presenter_static_init();
 if(OSMetaClass::postModLoad(handle))return;
 PE_Video v={};stage=2;
 if(PE_current_console(&v) || !v.v_baseAddr || !v.v_width || !v.v_height || v.v_depth!=32 || v.v_rotate ||
  v.v_width>16384 || v.v_height>16384 || v.v_rowBytes<uint64_t(v.v_width)*4 || v.v_rowBytes>UINT64_MAX/v.v_height)return;
 printf("SMGOP20 video pa%llx %lux%lu stride%lu depth%lu display%lu\n",(uint64_t)v.v_baseAddr,(unsigned long)v.v_width,(unsigned long)v.v_height,(unsigned long)v.v_rowBytes,(unsigned long)v.v_depth,(unsigned long)v.v_display);
 IOService *display=native_legacy_display_allocate();IOPlatformDevice *nub=new IOPlatformDevice;
 IOService *parent=IOService::getResourceService();IORegistryEntry *tree=IORegistryEntry::fromPath("/arm-io",gIODTPlane);
 IODeviceMemory *memory=IODeviceMemory::withRange(v.v_baseAddr,v.v_rowBytes*v.v_height);
 OSArray *ranges=OSArray::withCapacity(1);OSData *type=OSData::withBytes("display-subsystem",sizeof("display-subsystem"));
 bool na=false,ns=false,dt=false,da=false,ds=false;stage=3;
 if(display && nub && parent && tree && memory && ranges && type && display->init() && nub->init()) {
  ranges->setObject(memory);nub->setDeviceMemory(ranges);nub->setName("NativeGOPFramebuffer");
  na=nub->attach(parent);if(na)ns=nub->start(parent);
  if(ns && nub->setProperty("device_type",type)) {
   nub->setName("gop-display",gIODTPlane);dt=nub->attachToParent(tree,gIODTPlane);
  }
  SInt32 score=0;
  if(dt && display->probe(nub,&score)==display)da=display->attach(nub);
  if(da){stage=4;ds=display->start(nub);}
 }
 printf("SMGOP20 start nub%u dt%u attach%u start%u\n",ns?1U:0U,dt?1U:0U,da?1U:0U,ds?1U:0U);
 if(ds) {
  activeDisplay=display;activeNub=nub;stage=5;
  startIn2020Backlight();
  startCpuControlSample();
  startEpssSample();
  if(gpu==1) {
   IOService *handoff=native_gop_presenter_create();
   bool attached=false;
   if(handoff && handoff->init()) {
    native_gop_presenter_set_source(handoff,nub);
    attached=handoff->attach(parent);
    if(attached && handoff->start(parent))
     __atomic_store_n(&gpuHandoffPresenter,handoff,__ATOMIC_RELEASE);
   }
   if(!gpuHandoffPresenter) {
    if(attached)handoff->detach(parent);
    OSSafeReleaseNULL(handoff);
   }
   printf("SMGOP75 presenter ready=%u\n",gpuHandoffPresenter?1U:0U);
  }
 }
 else {
  if(da)display->detach(nub);if(dt)nub->detachFromParent(tree,gIODTPlane);
  if(ns)nub->stop(parent);if(na)nub->detach(parent);
  OSSafeReleaseNULL(display);OSSafeReleaseNULL(nub);
 }
 OSSafeReleaseNULL(tree);OSSafeReleaseNULL(type);OSSafeReleaseNULL(memory);OSSafeReleaseNULL(ranges);
}
}
