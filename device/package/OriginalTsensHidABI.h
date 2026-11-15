// SPDX-License-Identifier: MIT
// Target-qualified opaque declaration, not a copy of Apple header implementation.
#pragma once
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
class OSString;class OSArray;
class IOHIDEventService : public IOService {
 OSDeclareAbstractStructors(IOHIDEventService)
 unsigned char originalPrivate[200-sizeof(IOService)];
protected:
 const void *originalPointer(size_t offset) const {
  return *reinterpret_cast<const void *const *>(reinterpret_cast<const unsigned char *>(this)+offset);
 }
 bool originalExpansionPresent() const {return originalPointer(0xc0)!=nullptr;}
 bool originalInitResourcesPresent() const {
  const auto *exp=static_cast<const unsigned char *>(originalPointer(0xc0));
  return exp && originalPointer(0xa0) && originalPointer(0xa8) &&
   *reinterpret_cast<const void *const *>(exp+0x20) && *reinterpret_cast<const void *const *>(exp+0x30);
 }
public:
 kern_return_t Dispatch(IORPC) override;
 void free() override;
 bool init(OSDictionary * =nullptr) override;
 IOReturn setProperties(OSObject *) override;
 bool start(IOService *) override;
 void stop(IOService *) override;
 void close(IOService *,IOOptionBits) override;
 bool handleOpen(IOService *,IOOptionBits,void *) override;
 void handleClose(IOService *,IOOptionBits) override;
 bool handleIsOpen(const IOService *) const override;
 bool matchPropertyTable(OSDictionary *,SInt32 *) override;
 IOReturn message(UInt32,IOService *,void *) override;
 IOReturn newUserClient(task_t,void *,UInt32,OSDictionary *,IOUserClient **) override;
 virtual void originalSlot168();
 virtual void originalSlot169();
 virtual OSString *getTransport();
 virtual UInt32 getLocationID();
 virtual void originalSlot172();
 virtual void originalSlot173();
 virtual void originalSlot174();
 virtual void originalSlot175();
 virtual void originalSlot176();
 virtual OSString *getManufacturer();
 virtual OSString *getProduct();
 virtual void originalSlot179();
 virtual OSArray *getReportElements();
 virtual void originalSlot181();
 virtual void originalSlot182();
 virtual void originalSlot183();
 virtual void originalSlot184();
 virtual void originalSlot185();
 virtual void originalSlot186();
 virtual void originalSlot187();
 virtual void originalSlot188();
 virtual void originalSlot189();
 virtual void originalSlot190();
 virtual UInt32 getReportInterval();
 virtual void originalSlot192();
 virtual void originalSlot193();
 virtual void originalSlot194();
 virtual void originalSlot195();
 virtual void originalSlot196();
 virtual void originalSlot197();
 virtual void originalSlot198();
 virtual UInt32 getPrimaryUsagePage();
 virtual UInt32 getPrimaryUsage();
 virtual OSObject *copyEvent(UInt32,OSObject *,IOOptionBits);
 virtual void originalSlot202();
 virtual void originalSlot203();
 virtual void originalSlot204();
 virtual void originalSlot205();
 virtual void originalSlot206();
 virtual void originalSlot207();
 virtual void originalSlot208();
 virtual void originalSlot209();
 virtual void originalSlot210();
 virtual void originalSlot211();
 virtual void originalSlot212();
 virtual void originalSlot213();
 virtual void originalSlot214();
 virtual void originalSlot215();
 virtual void originalSlot216();
 virtual void originalSlot217();
 virtual void originalSlot218();
};
static_assert(sizeof(IOHIDEventService)==200,"original HID service storage");
