#pragma once
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
class IOWatchdog : public IOService {
 OSDeclareAbstractStructors(IOWatchdog)
 unsigned char originalPrivate[0xf8-sizeof(IOService)];
public:
 void stop(IOService *) override;
 IOReturn newUserClient(task_t, void *, UInt32, OSDictionary *, IOUserClient **) override;
 virtual void enableBackend() = 0;
 virtual void disableBackend() = 0;
 virtual void restartBackend() = 0;
};
static_assert(sizeof(IOWatchdog)==0xf8,"Original watchdog storage");
