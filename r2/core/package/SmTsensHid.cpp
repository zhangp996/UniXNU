// SPDX-License-Identifier: MIT
// Original IOHIDEventService ABI with physical SM8250 channel identity.
#include "OriginalTsensHidABI.h"
#include "SmTsensHidState.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOWorkLoop.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSIterator.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSSymbol.h>
#include <mach/kmod.h>
#include <sys/errno.h>
extern "C" OSObject *native_sm8250_thermal_copy_event(unsigned,unsigned,unsigned,uint64_t);
extern "C" int native_sm8250_thermal_live_copy(unsigned,unsigned,int32_t *,uint64_t *);
extern "C" unsigned native_sm8250_thermal_event_error(void);
extern "C" kern_return_t OSKextLoadKextWithIdentifier(const char *);
extern "C" kmod_info_t g_kernel_kmod_info;
extern "C" void sm_tsens_hid_static_init(void) __asm__("__GLOBAL__sub_I_SmTsensHid.cpp");
namespace {
// Query counters change on client threads, so they stay plain atomics and are
// read one by one; everything else is written only by the boot thread and
// published under one short lock (below) so a reader never mixes phases.
static uint64_t queries,answers,offline,refused;
static unsigned hid_prepare_state;
static int hid_create_error[2]={ENXIO,ENXIO};
static unsigned hid_created,hid_class_error;
static unsigned hid_group[2],hid_channel[2],hid_product_ok[2];
static unsigned hid_requested[2],hid_published[2],hid_matched[2];
static unsigned hid_event_probe[2];
static const char *hid_step="start";
static SmTsensHidState hid_state;
// The published state is ordinary memory, so both sides take this short
// critical section: a sequence counter plus fences would still be a data race
// on the fields themselves. The lock is allocated once, before anything is
// created, and only normal-context code (boot caller, sampler, boot printf)
// reads it - never the panic path.
// The sampler starts before HID prepare: publish this board-lifetime pointer
// with release/acquire only after both the lock and initial state are ready.
static IOSimpleLock *hid_state_lock;
static IOService *tsens_hid_endpoints[2];
static void publish_state(void) {
 IOSimpleLock *state_lock=__atomic_load_n(&hid_state_lock,__ATOMIC_ACQUIRE);
 if(!state_lock)return;
 IOInterruptState interrupts=IOSimpleLockLockDisableInterrupt(state_lock);
 hid_state.class_error=hid_class_error;
 hid_state.created=hid_created;
 hid_state.endpoint_error[0]=unsigned(hid_create_error[0]);
 hid_state.endpoint_error[1]=unsigned(hid_create_error[1]);
 for(unsigned i=0;i<2;++i) {
  hid_state.requested[i]=hid_requested[i];
  hid_state.event_probe[i]=hid_event_probe[i];
  hid_state.published[i]=hid_published[i];
  hid_state.matched[i]=hid_matched[i];
  hid_state.group[i]=hid_group[i];hid_state.channel[i]=hid_channel[i];
  hid_state.product_ok[i]=hid_product_ok[i];
  hid_state.location[i]=tsens_hid_endpoints[i]?(hid_group[i]?0x0c265000U:0x0c263000U)+0xa0+4*hid_channel[i]:0U;
 }
 hid_state.last_error=native_sm8250_thermal_event_error();
 hid_state.step=hid_step;
 IOSimpleLockUnlockEnableInterrupt(state_lock,interrupts);
}
static uint64_t begin_query(unsigned group,unsigned type,bool matching,
 IOOptionBits options,uint64_t caller) {
 IOSimpleLock *state_lock=__atomic_load_n(&hid_state_lock,__ATOMIC_ACQUIRE);
 if(!state_lock || group>=2)return 0;
 IOInterruptState interrupts=IOSimpleLockLockDisableInterrupt(state_lock);
 SmTsensHidQuery &q=hid_state.query[group];
 uint64_t ticket=++q.seen;
 if(type==15)++q.temperature;
 q.type=type;q.matching=matching;q.options=options;q.caller=caller;
 q.reason=SmTsensQueryPending;
 IOSimpleLockUnlockEnableInterrupt(state_lock,interrupts);
 return ticket;
}
static void end_query(unsigned group,uint64_t ticket,SmTsensHidQueryReason reason) {
 IOSimpleLock *state_lock=__atomic_load_n(&hid_state_lock,__ATOMIC_ACQUIRE);
 if(!state_lock || group>=2 || !ticket)return;
 IOInterruptState interrupts=IOSimpleLockLockDisableInterrupt(state_lock);
 SmTsensHidQuery &q=hid_state.query[group];
 if(reason==SmTsensQueryAnswered)++q.answered;else ++q.rejected;
 // A slower earlier request must not overwrite a newer request's shape/result.
 if(q.seen==ticket)q.reason=reason;
 IOSimpleLockUnlockEnableInterrupt(state_lock,interrupts);
}
}
class SmTsensHid : public IOHIDEventService {
 OSDeclareDefaultStructors(SmTsensHid)
 unsigned group=0,channel=0,online=0,publish_requested=0;
 IOOptionBits publish_options=0;
 bool entered=false;
 IOService *start_provider=nullptr;
 IOWorkLoop *loop=nullptr;
 OSString *product=nullptr,*transport=nullptr,*maker=nullptr;
public:
 bool init(OSDictionary *properties=nullptr) override {
  return IOHIDEventService::init(properties) && originalInitResourcesPresent();
 }
 bool configure(unsigned g,unsigned c) {
  if(g>=2 || c>=16 || loop || product || transport || maker)return false;
  group=g;channel=c;char name[64];
  snprintf(name,sizeof(name),"SM8250 TSENS%u channel%u",g,c);
  product=OSString::withCString(name);transport=OSString::withCString("MMIO");maker=OSString::withCString("Qualcomm");
  loop=IOWorkLoop::workLoop();
  // Record whether the identity string that will be published is exactly the
  // intended one; the panic note reports this instead of re-reading an OSObject.
  hid_product_ok[g]=product && product->isEqualTo(name);
  return product && transport && maker && loop &&
   setProperty("TSENSGroup",g,32) && setProperty("TSENSChannel",c,32) &&
   setProperty("TemperatureMaximumAgeMilliseconds",3000U,32);
 }
 IOWorkLoop *getWorkLoop() const override {return loop;}
 OSString *getTransport() override {return transport;}
 OSString *getManufacturer() override {return maker;}
 OSString *getProduct() override {return product;}
 UInt32 getLocationID() override {return (group?0x0c265000U:0x0c263000U)+0xa0+4*channel;}
 UInt32 getPrimaryUsagePage() override {return 0x20;}
 // Target 21H461 contract: HidSensors::createHIDEventSystemObject explicitly
 // matches 0x20/0x31 at 0x100010110..118. Public USB usage tables cannot replace
 // that private OS contract. Product/LocationID still name physical TSENS only.
 UInt32 getPrimaryUsage() override {return 0x31;}
 UInt32 getReportInterval() override {return 1000000;}
 OSArray *getReportElements() override {return nullptr;}
 // Original start requests publication as its last action, before it returns.
 // Holding it back keeps queries off a half-started endpoint; the request and
 // its option bits are preserved instead of inventing a new publication policy.
 // Publication stays synchronous because the physical board services already
 // published from this boot phase require MatchedState on lookup (H3V evidence).
 void registerService(IOOptionBits=0) override;
 bool start(IOService *provider) override;
 void stop(IOService *provider) override {
  __atomic_store_n(&online,0U,__ATOMIC_RELEASE);
  __atomic_store_n(&publish_requested,0U,__ATOMIC_RELEASE);
  if(entered) {entered=false;IOHIDEventService::stop(provider);start_provider=nullptr;}
 }
 OSObject *copyEvent(UInt32 type,OSObject *matching,IOOptionBits options) override {
  __atomic_add_fetch(&queries,1U,__ATOMIC_RELAXED);
  uint64_t ticket=begin_query(group,type,matching!=nullptr,options,
   reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
  // Original 21H461 thermalmonitord queries type15 with a NULL client match and
  // zero options; every other shape is refused rather than approximated.
  if(!__atomic_load_n(&online,__ATOMIC_ACQUIRE)) {
   __atomic_add_fetch(&offline,1U,__ATOMIC_RELAXED);
   end_query(group,ticket,SmTsensQueryOffline);return nullptr;
  }
  if(type!=15 || matching || options) {
   __atomic_add_fetch(&refused,1U,__ATOMIC_RELAXED);
   end_query(group,ticket,type!=15?SmTsensQueryType:
    matching?SmTsensQueryMatching:SmTsensQueryOptions);return nullptr;
  }
  OSObject *event=native_sm8250_thermal_copy_event(group,channel,type,getRegistryEntryID());
  if(event)__atomic_add_fetch(&answers,1U,__ATOMIC_RELAXED);
  else __atomic_add_fetch(&refused,1U,__ATOMIC_RELAXED);
  end_query(group,ticket,event?SmTsensQueryAnswered:SmTsensQueryBackend);
  return event;
 }
 void free() override {
  if(entered)stop(start_provider);
  // Original base owns a retained reference to the work loop after start.
  // Its free removes its command gate and releases that reference itself.
  if(loop) {loop->release();loop=nullptr;}
  if(product) {product->release();product=nullptr;}
  if(transport) {transport->release();transport=nullptr;}
  if(maker) {maker->release();maker=nullptr;}
  if(originalExpansionPresent())IOHIDEventService::free();
  else IOService::free(); // original init failed before allocating its private area
 }
};
void SmTsensHid::registerService(IOOptionBits options) {
 if(__atomic_load_n(&online,__ATOMIC_ACQUIRE)) {IOService::registerService(options);return;}
 publish_options=options;
 __atomic_store_n(&publish_requested,1U,__ATOMIC_RELEASE);
 hid_requested[group]=1U;
}
bool SmTsensHid::start(IOService *provider) {
 if(entered || !provider || !loop || !product || !transport || !maker)return false;
 entered=true;start_provider=provider;
 if(!IOHIDEventService::start(provider)) {stop(provider);return false;}
 __atomic_store_n(&online,1U,__ATOMIC_RELEASE);
 if(__atomic_load_n(&publish_requested,__ATOMIC_ACQUIRE)) {
  IOService::registerService((publish_options & ~IOOptionBits(kIOServiceAsynchronous)) | kIOServiceSynchronous);
  hid_published[group]=1U;
 }
 return true;
}
OSDefineMetaClassAndStructors(SmTsensHid,IOHIDEventService)
#include "In2020RingerService.h"
static bool tsens_hid_class_attempted,tsens_hid_class_ready;
// One boot-thread caller. Production caller must initialize the live backend
// first, load the original family and qualify the original metaclass identity.
extern "C" int native_sm8250_thermal_hid_register_class(void) {
 if(tsens_hid_class_attempted)return EALREADY;
 tsens_hid_class_attempted=true;
 if(OSKextLoadKextWithIdentifier("com.apple.iokit.IOHIDFamily"))return ENXIO;
 const OSSymbol *name=OSSymbol::withCString("IOHIDEventService");if(!name)return ENOMEM;
 const OSMetaClass *base=OSMetaClass::getMetaClassWithName(name);name->release();
 if(!base || base!=&IOHIDEventService::gMetaClass || base->getClassSize()!=200)return ENOEXEC;
 void *handle=OSMetaClass::preModLoad(g_kernel_kmod_info.name);if(!handle)return ENOMEM;
 sm_tsens_hid_static_init();
 if(OSMetaClass::postModLoad(handle))return ENOEXEC;
 __atomic_store_n(&tsens_hid_class_ready,true,__ATOMIC_RELEASE);return 0;
}
extern "C" IOService *native_sm8250_thermal_hid_create(unsigned group,unsigned channel) {
 if(!__atomic_load_n(&tsens_hid_class_ready,__ATOMIC_ACQUIRE))return nullptr;
 int32_t value;uint64_t timestamp;
 // Do not publish a channel that has not produced a valid fresh measurement.
 int error=native_sm8250_thermal_live_copy(group,channel,&value,&timestamp);
 if(error)return nullptr;
 IOService *provider=IOService::getResourceService();if(!provider)return nullptr;
 auto *service=new SmTsensHid;if(!service)return nullptr;
 if(!service->init() || !service->configure(group,channel)) {service->release();return nullptr;}
 if(!service->attach(provider)) {service->release();return nullptr;}
 if(!service->start(provider)) {service->detach(provider);service->release();return nullptr;}
 // Caller owns this reference and must terminate/stop, detach and release the
 // endpoint during removal. The shared acquisition backend remains boot-owned.
 return service;
}
// A successful start only means the object exists. Publish means this service
// asked the registry; matched means a lookup actually finds it.
static void note_matches(void) {
 OSDictionary *match=IOService::serviceMatching("SmTsensHid");
 if(!match)return;
 OSIterator *iterator=IOService::getMatchingServices(match);
 match->release();
 if(!iterator)return;
 while(OSObject *object=iterator->getNextObject())
  for(unsigned i=0;i<2;++i)if(object==tsens_hid_endpoints[i])hid_matched[i]=1U;
 iterator->release();
}
extern "C" int native_sm8250_thermal_hid_prepare(void) {
 if(__atomic_load_n(&hid_prepare_state,__ATOMIC_ACQUIRE))return EALREADY;
 __atomic_store_n(&hid_prepare_state,1U,__ATOMIC_RELEASE);
 hid_step="lock";
 IOSimpleLock *state_lock=IOSimpleLockAlloc();
 if(!state_lock)return ENOMEM;
 // No reader can reach hid_state until the pointer is published below.
 hid_state.step=hid_step;
 hid_state.endpoint_error[0]=unsigned(hid_create_error[0]);
 hid_state.endpoint_error[1]=unsigned(hid_create_error[1]);
 __atomic_store_n(&hid_state_lock,state_lock,__ATOMIC_RELEASE);
 publish_state();
 hid_step="class";
 hid_class_error=unsigned(native_sm8250_thermal_hid_register_class());
 publish_state();
 if(hid_class_error)return int(hid_class_error);
 native_in2020_ringer_prepare();
 static const unsigned channels[2][2]={{0,0},{1,0}};
 for(unsigned i=0;i<2;++i) {
  hid_step=i?"create1":"create0";
  hid_group[i]=channels[i][0];hid_channel[i]=channels[i][1];
  int32_t value;uint64_t timestamp;
  hid_create_error[i]=native_sm8250_thermal_live_copy(channels[i][0],channels[i][1],&value,&timestamp);
  if(!hid_create_error[i]) {
   tsens_hid_endpoints[i]=native_sm8250_thermal_hid_create(channels[i][0],channels[i][1]);
   if(tsens_hid_endpoints[i])++hid_created;
   else hid_create_error[i]=ENXIO;
  }
  publish_state();
 }
 if(!hid_created) {hid_step="no-endpoint";publish_state();return ENXIO;}
 hid_step="match";
 note_matches();publish_state();
 // Exercise the actual live adapter and original withBytes once per endpoint.
 // This is a kernel-only probe: do not route it through copyEvent or increment
 // any client query/answer counter, and do not pretend an Apple sensor matched.
 for(unsigned i=0;i<2;++i) {
  if(!tsens_hid_endpoints[i]) {hid_event_probe[i]=3;publish_state();continue;}
  hid_step=i?"probe1":"probe0";publish_state();
  OSObject *event=native_sm8250_thermal_copy_event(hid_group[i],hid_channel[i],15,
   tsens_hid_endpoints[i]->getRegistryEntryID());
  if(event) {event->release();hid_event_probe[i]=1;}
  else hid_event_probe[i]=2;
  publish_state();
 }
 hid_step="ready";publish_state();
 IOLog("SM8250 thermal: %u live HID temperature endpoints from physical TSENS channels\n",hid_created);
 return 0;
}
// Reader side: the same short critical section, so no field is read while it is
// being written. Normal context only.
extern "C" void native_sm8250_thermal_hid_state(SmTsensHidState *out) {
 if(!out)return;
 IOSimpleLock *state_lock=__atomic_load_n(&hid_state_lock,__ATOMIC_ACQUIRE);
 if(!state_lock) {
  *out=SmTsensHidState{};
  out->step="unavailable";
  return;
 }
 IOInterruptState interrupts=IOSimpleLockLockDisableInterrupt(state_lock);
 SmTsensHidState copy=hid_state;
 IOSimpleLockUnlockEnableInterrupt(state_lock,interrupts);
 *out=copy;
 out->last_error=native_sm8250_thermal_event_error();
 out->queries=__atomic_load_n(&queries,__ATOMIC_RELAXED);
 out->answered=__atomic_load_n(&answers,__ATOMIC_RELAXED);
 out->offline=__atomic_load_n(&offline,__ATOMIC_RELAXED);
 out->refused=__atomic_load_n(&refused,__ATOMIC_RELAXED);
}
extern "C" const char *native_sm8250_thermal_hid_note(void) {
 static char note[384];
 SmTsensHidState state;
 native_sm8250_thermal_hid_state(&state);
 snprintf(note,sizeof(note),
  "hid step%s clserr%u raw%u,%u ep%u/2 req%u%u pub%u%u mt%u%u loc%x,%x gc%u%u/%u%u prod%u/2 q%llu ok%llu off%llu bad%llu err%u\n",
  state.step,state.class_error,state.endpoint_error[0],state.endpoint_error[1],state.created,
  state.requested[0],state.requested[1],state.published[0],state.published[1],
  state.matched[0],state.matched[1],state.location[0],state.location[1],
  state.group[0],state.channel[0],state.group[1],state.channel[1],
  state.product_ok[0]+state.product_ok[1],
  static_cast<unsigned long long>(__atomic_load_n(&queries,__ATOMIC_RELAXED)),
  static_cast<unsigned long long>(__atomic_load_n(&answers,__ATOMIC_RELAXED)),
  static_cast<unsigned long long>(__atomic_load_n(&offline,__ATOMIC_RELAXED)),
  static_cast<unsigned long long>(__atomic_load_n(&refused,__ATOMIC_RELAXED)),
  state.last_error);
 return note;
}
