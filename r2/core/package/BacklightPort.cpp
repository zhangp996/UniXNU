// SPDX-License-Identifier: BSD-2-Clause-Patent
// Board backlight entry point.  The sibling IN2020 port keeps In2020Backlight (an IOService publishing
// AppleARMBacklight-conforming brightness properties over the DSI0 transport) inside an anonymous namespace in
// In2020BacklightService.h, so it can only be started from the translation unit that includes that header.  This
// file is that translation unit and exports a C entry point for the display probe to call.
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <kern/thread_call.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSSymbol.h>
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>
#include <sys/proc.h>

#include "In2020BacklightTransport.h"
#include "In2020BacklightService.h"

extern "C" void
native_board_backlight_start(void)
{
	startIn2020Backlight();
}
