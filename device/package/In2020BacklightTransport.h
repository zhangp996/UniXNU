// SPDX-License-Identifier: BSD-2-Clause-Patent
// IN2020 DSI transport, qualified by the brightness-trial-v2 hardware log.
namespace {
constexpr uint64_t kIrisReady = 0x0f970000; // Kona TLMM NORTH, GPIO112.
constexpr uint64_t kDisplayClockPage = 0x0af02000;

IOMemoryMap *displayProbeMap(uint64_t address, uint64_t bytes)
{
 IODeviceMemory *memory=IODeviceMemory::withRange(address,bytes);
 IOMemoryMap *map=memory?memory->map(kIOMapAnywhere|kIOMapInhibitCache|kIOMapReadOnly):nullptr;
 OSSafeReleaseNULL(memory);
 return map;
}

uint32_t displayProbeRead(IOMemoryMap *map,uint32_t offset)
{
 return *reinterpret_cast<const volatile uint32_t *>(map->getVirtualAddress()+offset);
}

bool displayClocksActive(IOMemoryMap *map)
{
 const uint32_t ahb=displayProbeRead(map,0x80), mdp=displayProbeRead(map,0x0c);
 constexpr uint32_t branchEnabled=1, branchOff=1U<<31;
 return (ahb&(branchEnabled|branchOff))==branchEnabled &&
        (mdp&(branchEnabled|branchOff))==branchEnabled;
}

void displayDsiWrite(IOMemoryMap *map,uint32_t offset,uint32_t value)
{
 *reinterpret_cast<volatile uint32_t *>(map->getVirtualAddress()+offset)=value;
 __asm__ volatile("dsb sy" ::: "memory");
}

bool displayBrightnessTransfer(IOMemoryMap *clocks,bool read,uint32_t level,
                               uint32_t &observed,bool &issued)
{
 issued=false;
 observed=0;
 IOMemoryMap *iris=displayProbeMap(kIrisReady,0x1000);
 const bool bypass=iris && !(displayProbeRead(iris,0)&((1U<<9)|(0xfU<<2))) &&
                  (displayProbeRead(iris,4)&1);
 OSSafeReleaseNULL(iris);
 constexpr uint32_t clockStateMask=(1U<<31)|1U;
 if(!bypass || !displayClocksActive(clocks) ||
    (displayProbeRead(clocks,0x28)&clockStateMask)!=1 ||
    (displayProbeRead(clocks,0x38)&clockStateMask)!=1) {
  printf("DISPLAY-DCS skipped=bypass-or-clocks\n");
  return false;
 }

 // The factory DTB selects command-mode DSI0 with a software DMA trigger.
 constexpr uint32_t versionReg=0,control=4,status=8,dmaControl=0x3c,dmaLength=0x4c;
 constexpr uint32_t ackStatus=0x68,readData=0x6c,triggerConfig=0x84,trigger=0x90;
 constexpr uint32_t timeoutStatus=0xc0,interrupts=0x110,tpg=0x15c,fifo=0x17c;
 constexpr uint32_t readControl=0x1d4,fifoReset=0x1ec;
 constexpr uint32_t dmaDone=1U,btaDone=1U<<20,irqError=1U<<24;
 constexpr uint32_t irqStatusMask=dmaDone|btaDone|irqError|(1U<<8)|(1U<<10)|(1U<<12)|
                                   (1U<<14)|(1U<<16)|(1U<<28)|(1U<<30);
 constexpr uint32_t busyMask=(1U<<31)|7U;
 IODeviceMemory *memory=IODeviceMemory::withRange(0x0ae94000,0x1000);
 IOMemoryMap *dsi=memory?memory->map(kIOMapAnywhere|kIOMapInhibitCache):nullptr;
 OSSafeReleaseNULL(memory);
 if(!dsi) {printf("DISPLAY-DCS skipped=dsi-map\n");return false;}

 const uint32_t version=displayProbeRead(dsi,versionReg),ctrl=displayProbeRead(dsi,control);
 const uint32_t triggerMode=displayProbeRead(dsi,triggerConfig),active=displayProbeRead(dsi,status);
 if(version!=0x20040000 || (ctrl&7)!=5 || (triggerMode&7)!=4) {
  printf("DISPLAY-DCS skipped=host-mode\n");
  dsi->release();
  return false;
 }

 // ColorOS waits up to 200 ms for command-mode MDP idle. Also require the
 // command engine and DMA idle because this boot does not own a DSI driver lock.
 uint64_t idleDeadline;
 clock_interval_to_deadline(200,NSEC_PER_MSEC,&idleDeadline);
 uint32_t idleStatus=active;
 unsigned polls=1;
 while((idleStatus&busyMask) && mach_absolute_time()<idleDeadline) {
  IOSleep(2);
  if((displayProbeRead(clocks,0x80)&clockStateMask)!=1 ||
     (displayProbeRead(clocks,0x28)&clockStateMask)!=1 ||
     (displayProbeRead(clocks,0x38)&clockStateMask)!=1) {
   printf("DISPLAY-DCS skipped=clocks-during-idle-wait\n");
   dsi->release();
   return false;
  }
  idleStatus=displayProbeRead(dsi,status);
  ++polls;
 }
 if(idleStatus&busyMask) {
  printf("DISPLAY-DCS skipped=idle-timeout first=%x last=%x polls=%u\n",active,idleStatus,polls);
  dsi->release();
  return false;
 }
 const uint32_t pattern=displayProbeRead(dsi,tpg),interruptState=displayProbeRead(dsi,interrupts);
 if(pattern || (interruptState&irqError) || (displayProbeRead(dsi,status)&busyMask)) {
  printf("DISPLAY-DCS skipped=pre-submit-state\n");
  dsi->release();
  return false;
 }

 const uint32_t savedDma=displayProbeRead(dsi,dmaControl),savedLength=displayProbeRead(dsi,dmaLength);
 const uint32_t irqConfig=interruptState&~irqStatusMask;
 displayDsiWrite(dsi,interrupts,irqConfig|(interruptState&(dmaDone|btaDone)));
 if(read) {
  displayDsiWrite(dsi,readControl,1);
  displayDsiWrite(dsi,readControl,0);
 }
 displayDsiWrite(dsi,tpg,(1U<<1)|(1U<<2)|(3U<<16));
 if(read) {
  // Embedded maximum-return-size and DCS read with BTA.
  displayDsiWrite(dsi,fifo,0x00370002);
  displayDsiWrite(dsi,fifo,0xa0060052);
 } else {
  // DCS long write, word count 3, last command, followed by command and BE level.
  displayDsiWrite(dsi,fifo,0xc0390003);
  displayDsiWrite(dsi,fifo,0xff000051U|((level>>8)<<8)|((level&0xff)<<16));
 }
 constexpr uint32_t broadcastAndMaster=(1U<<31)|(1U<<30);
 constexpr uint32_t lpMode=1U<<26;
 displayDsiWrite(dsi,dmaControl,(savedDma&~(broadcastAndMaster|lpMode))|(1U<<28)|(read?lpMode:0));
 displayDsiWrite(dsi,dmaLength,8);
 issued=true;
 displayDsiWrite(dsi,trigger,1);

 uint64_t deadline;
 clock_interval_to_deadline(200,NSEC_PER_MSEC,&deadline);
 uint32_t irq=0,count=0;
 do {
  irq=displayProbeRead(dsi,interrupts);
  count=displayProbeRead(dsi,readControl)>>16;
  if((irq&irqError) || ((irq&dmaDone) && (!read || count || (irq&btaDone))))break;
  IOSleep(1);
 } while(mach_absolute_time()<deadline);

 const uint32_t packet=displayProbeRead(dsi,readData);
 const uint32_t ack=displayProbeRead(dsi,ackStatus),timeout=displayProbeRead(dsi,timeoutStatus);
 const uint32_t endStatus=displayProbeRead(dsi,status);
 const uint32_t dataType=(packet>>24)&0x3f;
 const bool complete=(irq&dmaDone) && !(irq&irqError) && !ack && !(timeout&0x111);
 const bool reply=complete && read && count==4 && dataType==0x22;
 if(reply) observed=(packet>>8)&0xffff;
 // Follow the vendor FIFO cleanup, then restore the command DMA settings.
 displayDsiWrite(dsi,tpg,pattern);
 displayDsiWrite(dsi,fifoReset,1);
 IODelay(1);
 displayDsiWrite(dsi,fifoReset,0);
 displayDsiWrite(dsi,dmaLength,savedLength);
 displayDsiWrite(dsi,dmaControl,savedDma);
 displayDsiWrite(dsi,interrupts,irqConfig|(irq&(dmaDone|btaDone)));
 if(!(read?reply:complete))printf("DISPLAY-DCS op=%s level=%u complete=%u reply=%u irq=%x count=%u packet=%08x ack=%x timeout=%x status=%x\n",
        read?"read":"write",level,unsigned(complete),unsigned(reply),irq,count,packet,ack,timeout,endStatus);
 dsi->release();
 return read?reply:complete;
}

}
