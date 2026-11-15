/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Included once by SmTsensHid.cpp: shares the already qualified base ABI and
 * its single controlled late class-registration constructor, not a new ABI. */
#pragma once
#include "In2020RingerWire.h"
#include <IOKit/IOTimerEventSource.h>
#include <mach/mach_time.h>
#include <pexpert/pexpert.h>
extern "C" unsigned sm8250_alert_slider_position(void);
extern "C" OSObject *in2020_original_keyboard_event(uint64_t,UInt32,UInt32,Boolean,IOOptionBits);
class In2020RingerService : public IOHIDEventService {
 OSDeclareDefaultStructors(In2020RingerService)
 IOWorkLoop *loop=nullptr;
 IOTimerEventSource *timer=nullptr;
 OSString *product=nullptr,*transport=nullptr,*maker=nullptr;
 IOService *startProvider=nullptr;
 unsigned online=0,requested=0,published=0,queries=0;
 IOOptionBits publishOptions=0;
 int lastState=-1;
 bool entered=false,timerAdded=false;
 unsigned allocationFailures=0;
 static SInt32 integer(OSObject *event,unsigned field) {
  auto table=*reinterpret_cast<uintptr_t **>(event);
  return reinterpret_cast<SInt32 (*)(void *,UInt32,IOOptionBits)>(table[23])(event,field,0);
 }
 OSObject *event(unsigned position) {
  const int state=in2020_ringer_state(position);
  if(state<0)return nullptr;
  // Call the selected OS's actual factory: it owns absolute flags, pressCount,
  // longPress/clickSpeed and all remaining private keyboard fields.
  OSObject *result=in2020_original_keyboard_event(mach_absolute_time(),11,46,Boolean(state),0);
  if(!result)return nullptr;
  auto table=*reinterpret_cast<uintptr_t **>(result);
  // Original dispatchEvent uses this same slot31 setter. Query responses need
  // the sender too, because they do not pass through dispatchEvent.
  reinterpret_cast<void (*)(void *,uint64_t)>(table[31])(result,getRegistryEntryID());
  const UInt32 type=reinterpret_cast<UInt32 (*)(void *,IOOptionBits)>(table[18])(result,0);
  if(type!=3 || integer(result,0x30000)!=11 || integer(result,0x30001)!=46 ||
     integer(result,0x30002)!=in2020_ringer_state(position)) {
   result->release();return nullptr;
  }
  return result;
 }
 static void tick(OSObject *owner,IOTimerEventSource *source) {
  auto *self=static_cast<In2020RingerService *>(owner);
  if(!__atomic_load_n(&self->online,__ATOMIC_ACQUIRE))return;
  const unsigned position=sm8250_alert_slider_position();
  const int state=in2020_ringer_state(position);
  if(state>=0) {
   if(!self->published && self->requested) {
    self->published=1;
    self->IOService::registerService(self->publishOptions);
    IOLog("RINGER ready page=b usage=2e position=%u state=%d continuous=1 factory=native\n",position,state);
   }
   if(self->published && self->lastState!=state) {
    OSObject *e=self->event(position);
    if(e) {
     // Slot198 is the original dispatchEvent(IOHIDEvent *,IOOptionBits).
     auto table=*reinterpret_cast<uintptr_t **>(self);
     reinterpret_cast<void (*)(void *,OSObject *,IOOptionBits)>(table[198])(self,e,0);
     e->release();self->lastState=state;
     IOLog("RINGER dispatch position=%u state=%d page=b usage=2e\n",position,state);
    } else if(++self->allocationFailures<=3) {
     IOLog("RINGER event allocation failed; retaining previous state and retrying\n");
    }
   }
  }
  source->setTimeoutMS(100);
 }
public:
 bool init(OSDictionary *properties=nullptr) override {
  return IOHIDEventService::init(properties) && originalInitResourcesPresent();
 }
 bool configure() {
  loop=IOWorkLoop::workLoop();
  product=OSString::withCString("OnePlus 8 Pro alert slider");
  transport=OSString::withCString("I2C");maker=OSString::withCString("OnePlus");
  if(!loop || !product || !transport || !maker)return false;
  timer=IOTimerEventSource::timerEventSource(this,tick);
  if(!timer || loop->addEventSource(timer)!=kIOReturnSuccess)return false;
  timerAdded=true;
  return setProperty("Built-In",true) && setProperty("In2020AlertSlider",true);
 }
 IOWorkLoop *getWorkLoop() const override {return loop;}
 OSString *getTransport() override {return transport;}
 OSString *getManufacturer() override {return maker;}
 OSString *getProduct() override {return product;}
 UInt32 getLocationID() override {return 0x984000;}
 UInt32 getPrimaryUsagePage() override {return 11;}
 UInt32 getPrimaryUsage() override {return 1;}
 UInt32 getReportInterval() override {return 100000;}
 OSArray *getReportElements() override {return nullptr;}
 void registerService(IOOptionBits options=0) override {
  publishOptions=options;requested=1;
  // Initial source may not be ready yet. Tick publishes only after the first
  // confirmed physical position; no default zero is exposed as a measurement.
 }
 bool start(IOService *provider) override {
  if(entered || !provider || !timer || !loop)return false;
  entered=true;startProvider=provider;
  if(!IOHIDEventService::start(provider)) {stop(provider);return false;}
  __atomic_store_n(&online,1U,__ATOMIC_RELEASE);
  timer->setTimeoutMS(100);return true;
 }
 OSObject *copyEvent(UInt32 type,OSObject *matching,IOOptionBits options) override {
  if(!__atomic_load_n(&online,__ATOMIC_ACQUIRE) || type!=3 || options)return nullptr;
  if(matching && (!matching->metaCast("IOHIDEvent") ||
    integer(matching,0x30000)!=11 || integer(matching,0x30001)!=46))return nullptr;
  const unsigned position=sm8250_alert_slider_position();
  if(in2020_ringer_state(position)<0)return nullptr;
  OSObject *result=event(position);
  const unsigned count=__atomic_add_fetch(&queries,1U,__ATOMIC_RELAXED);
  if(count<=4)IOLog("RINGER query type=3 matching=%u position=%u state=%d answered=%u\n",
   matching!=nullptr,position,in2020_ringer_state(position),result!=nullptr);
  return result;
 }
 void stop(IOService *provider) override {
  __atomic_store_n(&online,0U,__ATOMIC_RELEASE);
  if(timer)timer->cancelTimeout();
  if(entered) {entered=false;IOHIDEventService::stop(provider);startProvider=nullptr;}
 }
 void free() override {
  if(entered)stop(startProvider);
  if(timer) {timer->cancelTimeout();if(loop && timerAdded)loop->removeEventSource(timer);timer->release();timer=nullptr;timerAdded=false;}
  OSSafeReleaseNULL(loop);OSSafeReleaseNULL(product);OSSafeReleaseNULL(transport);OSSafeReleaseNULL(maker);
  if(originalExpansionPresent())IOHIDEventService::free();else IOService::free();
 }
};
OSDefineMetaClassAndStructors(In2020RingerService,IOHIDEventService)
static void native_in2020_ringer_prepare(void) {
 unsigned mode=0;
 if(!PE_parse_boot_argn("sm8250-slider-probe",&mode,sizeof(mode)) || mode!=4)return;
 IOService *provider=IOService::getResourceService();
 if(!provider) {IOLog("RINGER failed stage=provider\n");return;}
 auto *service=new In2020RingerService;
 if(!service) {IOLog("RINGER failed stage=allocate\n");return;}
 if(!service->init() || !service->configure()) {service->release();IOLog("RINGER failed stage=init\n");return;}
 if(!service->attach(provider)) {service->release();IOLog("RINGER failed stage=attach\n");return;}
 if(!service->start(provider)) {service->detach(provider);service->release();IOLog("RINGER failed stage=start\n");return;}
 // Board-lifetime reference; stop/free still cancel the timer before teardown.
 IOLog("RINGER service started; waiting for a confirmed physical position\n");
}
