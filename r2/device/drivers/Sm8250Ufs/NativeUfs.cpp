// SPDX-License-Identifier: BSD-2-Clause-Patent
// SM8250 (kona) UFS cold start, stage 1: controller clocks, block reset, PHY and link readiness.
//
// ShengUfs assumes the firmware has already brought UFS up; on this boot chain nothing does. The
// vendor UFSDxe fails with DEVICE_ERROR (its trusted-zone app cannot be read without UFS), so this
// driver supplies the missing cold start from the device's own vendor kernel sequence:
//
//   phy-qcom-ufs-qmp-v4.c      PHY power control, calibration tables, SerDes start, PCS poll
//   ufs-qcom.c                 GCC_UFS_PHY_BCR pulse, REG_UFS_CFG1 PHY soft reset, QUNIPRO select
//   gcc-kona.c                 the nine UFS branch registers and the ufs_phy_gdsc power domain
//
// It is probe-only: it programs the hardware, reports what came up, and never touches the request
// list or publishes a block device. The class keeps the name NativeUfs so the existing
// KernelConfigTables personality applies. The probe runs from a kernel thread after a delay: at
// root-device start the framebuffer console does not exist yet, so a panic there has nowhere to
// print and the device just resets.
//
// There is no serial console on this board, so a probe run ends in a panic whose message is the
// report. The panic screen is the only reliable channel.

#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/storage/IOBlockStorageDevice.h>
#include <IOKit/storage/IOStorage.h>
#include <IOKit/IOLib.h>
#include <libkern/libkern.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSString.h>
#include <kern/clock.h>
#include <kern/debug.h>
#include <kern/task.h>
#include <mach/mach_time.h>
#include <stdarg.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>

#include "Sm8250Facts.h"
#include "SmmuTakeover.h"
#include "Probe59Transport.h"
#include "Probe61Gpt.h"
#include "Probe62Inventory.h"
#include "Probe64OwnedDma.h"
#include "BlockReadCore.h"

#define SMUFS_LOG(fmt, ...) IOLog("SMUFS: " fmt "\n", ##__VA_ARGS__)

namespace {

inline void writeReg(uintptr_t base, uint32_t offset, uint32_t value)
{
	*(volatile uint32_t *)(base + offset) = value;
	__asm__ volatile("dsb sy" ::: "memory");
}

inline uint32_t readReg(uintptr_t base, uint32_t offset)
{
	return *(volatile uint32_t *)(base + offset);
}

// Everything the report shows, captured twice: once as the firmware left it, once after the
// cold start. Identical in a probe=1 run (no writes at all).
struct SmUfsSnapshot {
	uint32_t gdsc;
	uint32_t axi, ahb, unipro, aggre, aux, ice, tx0, rx0, rx1;
	uint32_t cap, ver, hcs, hce, rsr, utrlba, dbr, is, cfg1;
	uint32_t phyStart, phyPower, phyReset, pcs, laneCfg, multiLane;
};

// A clock root as the clock framework sees it: cmd/cfg plus the optional M/N/D divider legs.
struct SmUfsRcg {
	uint32_t cmd, cfg, m, n, d;
};

struct SmUfsState {
	SmUfsSnapshot pre, post;
	uint32_t level;
	uint32_t delayMs;
	uint32_t stage;      // last cold-start step reached
	uint32_t result;     // 0 ok, otherwise the failing step
	uint32_t phyResult;  // 0 = PCS ready
	uint32_t pcsPolls;
	// Stage 3: request-list takeover and the identification commands.
	uint32_t takeResult;
	uint32_t progress;      // last takeover step entered, for the watchdog report
	uint32_t progressDone;  // set once the probe is about to report
	uint32_t rsrBefore, rsrAfter, hcsAfter, dbrAfter, dmaPhys;
	uint32_t nopOcs, nopResp, nopScsi;
	uint32_t wakeOcs, wakeResp, wakeScsi;
	uint32_t capOcs, capResp, capScsi, capLastLba, capBlockLen;
	uint32_t readOcs, readResp, readScsi, readW0, readW1, readGpt0, readGpt1;
	uint32_t utrdCheck[8];
	uint32_t uicResult, uicHcs;
	uint32_t clkAxi, clkAgg, clkUnip, clkIce;
	// Read-only apps-SMMU inventory: does any stream match the UFS host, and how is it routed.
	uint32_t smmuScr0, smmuAcr, smmuIdr0, smmuIdr1, smmuValid, smmuS60, smmuS2c60, smmuIdx60;
	uint32_t smmuSid1, smmuMask1, smmuS2cr1, smmuCbar1, smmuSctlr1, smmuTtbr01, smmuIdx1;
	uint32_t bypassResult, bypassSid, bypassCbar, bypassScr0;
	uint32_t bypassS2crBefore, bypassS2crAfter, bypassFsr, bypassGfsr;
	uint32_t smmuPages;
	uint32_t iceVersion, iceCtrl, iceBypass, iceBist, ah8Timer, cfg2;
	uint32_t timC0, timC4, timD8;
	uint32_t ah8Cfg;
	// Clock roots: the state the firmware left, the state after the force-enable attempt, and the
	// state just before the first command. fix is 0 already running, 1 held by the branch request,
	// 2 never came back, 3 kept up only by the software force.
	SmUfsRcg rcgBefore[4], rcgFixed[4], rcgLate[4];
	uint32_t rcgFix[4], rcgFixPolls[4], rcgUpdate[4], rcgForced;
	uintptr_t gccBase, phyBase, hcBase;
	uint32_t uicErr[5];
	uint32_t uicIsBefore, uicIsAfter, uicReturns;
	uint32_t hwVersion, cfg1Before, cfg1After;
	uint32_t iceAdvBefore, iceAdvAfter, iceBistFinal, iceBistPolls;
	// Level 9: ShengUfs adoptAndRead minimal takeover.
	uint32_t minStep, minHcsBefore, minHcsAfter, minHce, minIsBefore, minIsAfter;
	uint32_t minDbrBefore, minDbrAfter, minTmrlBefore, minRsrBefore, minRsrAfter;
	uint32_t minOcs, minRsp, minScsi, minPolls, minFatal;
	uint64_t minList;
	uint32_t nocPrio, nocMode, nocBw, nocSat, aggreCbcr;
	uint32_t pwrDone, pwrRes, pwrMode;
	uint32_t wildResult, wildSlot, wildBank, wildSmr, wildS2cr, wildCbar, wildCba2r, wildSctlr;
	// Level 15: the kebab driver's takeover, on XNU's own contiguous DMA.
	uint32_t kbStep, kbHce, kbHcs, kbIs, kbDbr, kbRsr, kbTmrl;
	uint32_t kbIsAfter, kbDbrAfter;
	uint32_t kbTurOcs, kbTurRsp, kbTurScsi, kbTurTries, kbTurSense;
	uint32_t kbCapOcs, kbCapRsp, kbCapScsi, kbCapLast, kbCapBlk;
	uint32_t kbReadOcs, kbReadRsp, kbReadScsi, kbReadW0, kbReadW1, kbGpt0, kbGpt1;
	uint64_t kbDmaPa;
	uint32_t kbDmaContig;
	// Level 26 (pure adopt): the bootloader's list base and the slot we borrowed.
	uint64_t kbUtrlba;
	uint32_t kbPureSlot;
	uint32_t kbPureDw0;
	// Level 27: the vendor cold sequence's poked values, read back.
	uint32_t kbPokePcs, kbPokeIce, kbPokeCfg1;
	// Level 34: streams flipped to bypass, or 0xffffffff when the SMMU could not be mapped.
	uint32_t kbSmmuUfs;
	// Level 39: 1 when the second IORT stream (0x4e0) was installed as well.
	uint32_t kbSmmuStream2;
	// Level 16: the same takeover plus the three environmental differences still untested: the
	// descriptor allocation is forced below 4 GiB, the controller's clock-request lines are
	// enabled, and an ANOC1 wildcard stream bypass is installed, all before the first doorbell.
	uint32_t kbPhase, kbDmaLow, kbCapReg, kbClkReqBefore, kbClkReqAfter, kbFb2;
	// Level 17: ring from a helper thread so a store that never retires only wedges that thread;
	// the probe thread panics from memory within 50 ms, before the interconnect takes the display
	// with it. Levels 18/19/20 drop one extra each (clk_req, wildcard, low allocation).
	uint32_t kbRingDbr, kbRingIs, kbRingOcs, kbRingRsp, kbRingScsi, kbRingSense;
	uint32_t kbRingStore, kbRingPoll;
	uint32_t kbHiberResult, kbHiberHcs, kbHiberIs;
	uint32_t kbColdResult, kbLinkResult, kbLinkHcs, kbLinkIs;
};

// Framebuffer progress markers. The console is not a reliable channel during the takeover: a
// wedged bus stops the panic screen and the panel may be blanked by then. DarwinBoot reports the
// scanout at base 0x52000000, stride 4320 bytes (1080x2276, 32 bpp); one white square per completed
// step stays visible in a photograph even when nothing else gets drawn.
#define SM_FB_BASE   0x52000000ULL
#define SM_FB_STRIDE 4320U
#define SM_FB_STEPS  8

struct SmUfsFb {
	uintptr_t base;
	uint32_t stride;
	bool ok;
};

SmUfsFb gFb;
OSSharedPtr<IOMemoryMap> gFbMap;

void fbFill(const SmUfsFb &fb, uint32_t x0, uint32_t y0, uint32_t width, uint32_t height, uint32_t pixel)
{
	if (!fb.ok) {
		return;
	}
	for (uint32_t y = y0; y < y0 + height; y++) {
		volatile uint32_t *row = (volatile uint32_t *)(fb.base + (uintptr_t)y * fb.stride);
		for (uint32_t x = x0; x < x0 + width; x++) {
			row[x] = pixel;
		}
	}
	__asm__ volatile("dsb sy" ::: "memory");
}

// A white square proves the mapping; the green squares count the completed takeover steps.
// The row sits at mid-screen so the kernel console text, which grows from the top, does not
// overwrite it before the driver reports.
void fbMark(uint32_t step)
{
	fbFill(gFb, 16, 800, 24, 24, 0xffffffffU);
	for (uint32_t index = 0; index < SM_FB_STEPS; index++) {
		uint32_t pixel = (index < step) ? 0xff00ff00U : 0x00000000U;
		fbFill(gFb, 64 + index * 32, 800, 24, 24, pixel);
	}
}

// Second row for the fine-grained doorbell steps: 1 before the status clear, 2 after it, 3 after
// the doorbell write, 4 after the poll returns. One colour per step so a frozen frame still shows
// how far the submission got.
void fbMark2(uint32_t step)
{
	fbFill(gFb, 16, 848, 24, 24, 0xffffffffU);
	for (uint32_t index = 0; index < 4; index++) {
		uint32_t pixel = (index < step) ? 0xff00ff00U : 0x00000000U;
		fbFill(gFb, 64 + index * 32, 848, 24, 24, pixel);
	}
}

// DMA layout for the takeover path: [0,4096) request list (slot 0), [4096,5120) command
// UPIU + response UPIU + PRDT, [8192,16384) data. Same layout the sheng transport uses.
enum : uint32_t {
	kDmaBytes   = 16384,
	kUtmOffset  = 4096,
	kUcdOffset  = 8192,
	kRspOffset  = 512,
	kPrdtOffset = 1024,
	kDataOffset = 12288,
};

// The takeover reuses the request-list region the bootloader left running, as the sheng
// transport does: that memory is already reachable for the controller, while a fresh
// allocation is only reachable if the bootloader's mapping covers it.
struct SmUfsDma {
	OSSharedPtr<IOMemoryMap> map;
	uint8_t *virt;
	uint64_t phys;
};

struct SmUfsCommandResult {
	uint32_t ocs;
	uint32_t response;
	uint32_t scsi;
	uint32_t sense;   // sense key << 8 | ASC, from the response UPIU's sense data
};

struct SmUfsMaps {
	uintptr_t gcc, hc, phy;
	OSSharedPtr<IOMemoryMap> gccMap, hcMap, phyMap;
};

SmUfsState gSmUfs;

// Level 23b: the HIBER_EXIT result must be visible before the doorbell, because a stalled store
// kills the panic path. Three blocks at x=192/224/256, y=848 (past fbMark2's four slots):
// green=success, red=failure, white=not sent; then HCS.DP and UIC_ERROR.
void fbHibMark(void)
{
	const uint32_t white = 0xffffffffU, green = 0xff00ff00U, red = 0xffff0000U;
	uint32_t hib = gSmUfs.kbHiberResult;
	uint32_t c1 = hib == 0xee ? white : (hib == 0 ? green : red);
	uint32_t c2 = hib == 0xee ? white : ((gSmUfs.kbHiberHcs & 1) ? green : red);
	uint32_t c3 = hib == 0xee ? white : ((gSmUfs.kbHiberIs & SM_UFS_IS_UIC_ERROR) ? red : green);
	fbFill(gFb, 192, 848, 24, 24, c1);
	fbFill(gFb, 224, 848, 24, 24, c2);
	fbFill(gFb, 256, 848, 24, 24, c3);
}

// Level 25: full cold start and link result, drawn before the first doorbell.
void fbFullMark(void)
{
	const uint32_t green = 0xff00ff00U, red = 0xffff0000U;
	fbFill(gFb, 192, 848, 24, 24, gSmUfs.kbColdResult == 0 ? green : red);
	fbFill(gFb, 224, 848, 24, 24, gSmUfs.kbLinkResult == 0 ? green : red);
	fbFill(gFb, 256, 848, 24, 24, (gSmUfs.kbLinkHcs & 1) ? green : red);
}

// DMA-visible memory: the request list and its buffers are ordinary cacheable memory, as the
// reference transport maps the firmware list. A device (kIOMapInhibitCache) mapping of the same
// pages does not match the cache attributes the controller expects and can wedge the bus.
uint8_t *mapDma(uint64_t phys, uint64_t length, OSSharedPtr<IOMemoryMap> &keep)
{
	IOMemoryDescriptor *region = IOMemoryDescriptor::withPhysicalAddress(phys, length, kIODirectionInOut);
	if (region == nullptr) {
		return nullptr;
	}
	keep = OSSharedPtr<IOMemoryMap>(region->map(kIOMapAnywhere), OSNoRetain);
	region->release();
	return keep ? reinterpret_cast<uint8_t *>(keep->getVirtualAddress()) : nullptr;
}

uintptr_t mapPhys(uint64_t phys, uint64_t length, OSSharedPtr<IOMemoryMap> &keep)
{
	IODeviceMemory *memory = IODeviceMemory::withRange(phys, length);
	if (memory == nullptr) {
		return 0;
	}
	keep = OSSharedPtr<IOMemoryMap>(memory->map(kIOMapAnywhere | kIOMapInhibitCache), OSNoRetain);
	memory->release();
	return keep ? static_cast<uintptr_t>(keep->getVirtualAddress()) : 0;
}

void snapshot(const SmUfsMaps &map, SmUfsSnapshot &s)
{
	s.gdsc   = readReg(map.gcc, (uint32_t)(SM_UFS_PHY_GDSC - SM_GCC_BASE));
	s.axi    = readReg(map.gcc, SM_GCC_UFS_AXI_CBCR);
	s.ahb    = readReg(map.gcc, SM_GCC_UFS_AHB_CBCR);
	s.unipro = readReg(map.gcc, SM_GCC_UFS_UNIPRO_CORE_CBCR);
	s.aggre  = readReg(map.gcc, SM_GCC_AGGRE_UFS_AXI_CBCR);
	s.aux    = readReg(map.gcc, SM_GCC_UFS_PHY_AUX_CBCR);
	s.ice    = readReg(map.gcc, SM_GCC_UFS_ICE_CORE_CBCR);
	s.tx0    = readReg(map.gcc, SM_GCC_UFS_TX_SYMBOL0_CBCR);
	s.rx0    = readReg(map.gcc, SM_GCC_UFS_RX_SYMBOL0_CBCR);
	s.rx1    = readReg(map.gcc, SM_GCC_UFS_RX_SYMBOL1_CBCR);
	s.cap    = readReg(map.hc, SM_UFS_REG_CAP);
	s.ver    = readReg(map.hc, SM_UFS_REG_VERSION);
	s.hcs    = readReg(map.hc, SM_UFS_REG_HCS);
	s.hce    = readReg(map.hc, SM_UFS_REG_HCE);
	s.rsr    = readReg(map.hc, SM_UFS_REG_UTRLRSR);
	s.utrlba = readReg(map.hc, SM_UFS_REG_UTRLBA);
	s.dbr    = readReg(map.hc, SM_UFS_REG_UTRLDBR);
	s.is     = readReg(map.hc, SM_UFS_REG_IS);
	s.cfg1   = readReg(map.hc, SM_UFS_REG_CFG1);
	s.phyStart = readReg(map.phy, SM_PHY_START);
	s.phyPower = readReg(map.phy, SM_PHY_POWER_DOWN);
	s.phyReset = readReg(map.phy, SM_PHY_SW_RESET);
	s.pcs      = readReg(map.phy, SM_PHY_PCS_READY);
	s.laneCfg  = readReg(map.phy, SM_PHY_LINECFG_DISABLE);
	s.multiLane = readReg(map.phy, SM_PHY_MULTI_LANE_CTRL);
}

// gdsc_enable of the qcom gdsc driver: clear SW_COLLAPSE and wait for PWR_ON.
int powerGdsc(uintptr_t gcc)
{
	uint32_t offset = (uint32_t)(SM_UFS_PHY_GDSC - SM_GCC_BASE);
	uint32_t value = readReg(gcc, offset);
	writeReg(gcc, offset, value & ~SM_UFS_PHY_GDSC_SW_COLLAPSE);

	for (int i = 0; i < 1000; i++) {
		if (readReg(gcc, offset) & SM_UFS_PHY_GDSC_PWR_ON) {
			return 0;
		}
		IODelay(10);
	}
	return 1;
}

int enableBranch(uintptr_t gcc, uint32_t offset)
{
	uint32_t value = readReg(gcc, offset);
	writeReg(gcc, offset, value | SM_GCC_BRANCH_ENABLE);
	// hwcg_bit=1 branches report CLK_OFF while idle, so CLK_OFF must not be read as failure.
	return (readReg(gcc, offset) & SM_GCC_BRANCH_ENABLE) ? 0 : 1;
}

static const uint32_t kRcgOffsets[4] = {
	SM_GCC_UFS_AXI_RCG, SM_GCC_UFS_UNIPRO_RCG, SM_GCC_UFS_ICE_RCG, SM_GCC_UFS_PHY_AUX_RCG,
};
static const char * const kRcgNames[4] = {"axi", "unip", "ice", "aux"};

void readRcg(uintptr_t gcc, uint32_t offset, SmUfsRcg &out)
{
	out.cmd = readReg(gcc, offset + SM_RCG_CMD);
	out.cfg = readReg(gcc, offset + SM_RCG_CFG);
	out.m = readReg(gcc, offset + SM_RCG_M);
	out.n = readReg(gcc, offset + SM_RCG_N);
	out.d = readReg(gcc, offset + SM_RCG_D);
}

bool rcgRootOn(const SmUfsRcg &rcg)
{
	return (rcg.cmd & SM_RCG_CMD_ROOT_OFF) == 0;
}

// clk_rcg2_set_force_enable: assert the software root force and wait for ROOT_OFF to clear. The
// hardware gives it 500 microseconds; the same bound is used here so a stuck root is reported as
// stuck instead of being waited on forever.
int forceRcgRoot(uintptr_t gcc, uint32_t offset, uint32_t &polls)
{
	writeReg(gcc, offset + SM_RCG_CMD, readReg(gcc, offset + SM_RCG_CMD) | SM_RCG_CMD_ROOT_EN);
	for (polls = 0; polls < 500; polls++) {
		if ((readReg(gcc, offset + SM_RCG_CMD) & SM_RCG_CMD_ROOT_OFF) == 0) {
			return 0;
		}
		IODelay(1);
	}
	return 1;
}

// clk_rcg2_enable for an enable_safe_config root: force it on, run one configuration update, then
// drop the force and let the enabled branch hold it up. A root that is off here has no clock for
// the controller's request list fetch, and a doorbell write then waits for a fetch that never
// happens. When the branch request does not hold the root on, the force is put back so the next
// step still has a clock.
uint32_t repairRcgRoots(uintptr_t gcc)
{
	uint32_t forced = 0;
	for (unsigned index = 0; index < 4; index++) {
		uint32_t offset = kRcgOffsets[index];
		readRcg(gcc, offset, gSmUfs.rcgBefore[index]);
		gSmUfs.rcgFixed[index] = gSmUfs.rcgBefore[index];
		gSmUfs.rcgFixPolls[index] = 0;
		gSmUfs.rcgFix[index] = 0;
		gSmUfs.rcgUpdate[index] = 0;
		if (rcgRootOn(gSmUfs.rcgBefore[index])) {
			continue;
		}
		forced++;
		uint32_t polls = 0;
		if (forceRcgRoot(gcc, offset, polls) != 0) {
			gSmUfs.rcgFix[index] = 2;
			gSmUfs.rcgFixPolls[index] = polls;
			readRcg(gcc, offset, gSmUfs.rcgFixed[index]);
			continue;
		}
		// gcc-kona gives every one of these roots HW_CLK_CTRL_MODE, so __clk_rcg2_configure always
		// writes bit 20; a root left at reset never had it. Then run update_config's handshake.
		uint32_t cfg = readReg(gcc, offset + SM_RCG_CFG);
		if ((cfg & SM_RCG_CFG_HW_CTRL) == 0) {
			writeReg(gcc, offset + SM_RCG_CFG, cfg | SM_RCG_CFG_HW_CTRL);
		}
		writeReg(gcc, offset + SM_RCG_CMD,
		         readReg(gcc, offset + SM_RCG_CMD) | SM_RCG_CMD_UPDATE);
		gSmUfs.rcgUpdate[index] = 2;
		for (uint32_t wait = 0; wait < 500; wait++) {
			if ((readReg(gcc, offset + SM_RCG_CMD) & SM_RCG_CMD_UPDATE) == 0) {
				gSmUfs.rcgUpdate[index] = 1;
				break;
			}
			IODelay(1);
		}
		uint32_t forcedCmd = readReg(gcc, offset + SM_RCG_CMD);
		writeReg(gcc, offset + SM_RCG_CMD, forcedCmd & ~SM_RCG_CMD_ROOT_EN);
		IODelay(10);
		if (readReg(gcc, offset + SM_RCG_CMD) & SM_RCG_CMD_ROOT_OFF) {
			writeReg(gcc, offset + SM_RCG_CMD, forcedCmd | SM_RCG_CMD_ROOT_EN);
			gSmUfs.rcgFix[index] = 3;
		} else {
			gSmUfs.rcgFix[index] = 1;
		}
		gSmUfs.rcgFixPolls[index] = polls;
		readRcg(gcc, offset, gSmUfs.rcgFixed[index]);
	}
	return forced;
}

void reportRcgRoots(void)
{
	for (unsigned index = 0; index < 4; index++) {
		const SmUfsRcg &before = gSmUfs.rcgBefore[index];
		const SmUfsRcg &fixed = gSmUfs.rcgFixed[index];
		kprintf("SMUFS rcg %s cmd %08x>%08x cfg %08x>%08x src %u div %u m %x n %x d %x fix %u upd %u polls %u\n",
		        kRcgNames[index], before.cmd, fixed.cmd, before.cfg, fixed.cfg,
		        (before.cfg & SM_RCG_CFG_SRC_MASK) >> SM_RCG_CFG_SRC_SHIFT,
		        before.cfg & SM_RCG_CFG_DIV_MASK, before.m, before.n, before.d,
		        gSmUfs.rcgFix[index], gSmUfs.rcgUpdate[index], gSmUfs.rcgFixPolls[index]);
	}
	kprintf("SMUFS rcg forced %u\n", gSmUfs.rcgForced);
}

// Acknowledge the UIC error state the way the vendor ISR does: read IS, write it back to clear
// the latched bits, then take the five layer error codes. Reading them after the clear is the
// vendor order (ufshcd_intr writes IS before ufshcd_check_errors reads the codes). The poll then
// separates a stale latch from an error source that is still active.
void acknowledgeUicErrors(uintptr_t hc, bool poll)
{
	gSmUfs.uicIsBefore = readReg(hc, SM_UFS_REG_IS);
	writeReg(hc, SM_UFS_REG_IS, 0xffffffffU);
	for (unsigned index = 0; index < 5; index++) {
		gSmUfs.uicErr[index] = readReg(hc, SM_UFS_REG_UIC_ERR_PA + 4 * index);
	}
	// 0xffffffff means "not polled", so a level 4 report cannot be read as "cleared".
	gSmUfs.uicIsAfter = 0xffffffffU;
	gSmUfs.uicReturns = 0;
	if (!poll) {
		return;
	}
	for (unsigned loop = 0; loop < 10; loop++) {
		IODelay(1000);
		gSmUfs.uicIsAfter = readReg(hc, SM_UFS_REG_IS);
		if (gSmUfs.uicIsAfter & SM_UFS_IS_UIC_ERROR) {
			gSmUfs.uicReturns = 1;
		}
	}
}

void writeTable(uintptr_t phy, const struct SmPhiWrite *table, size_t count)
{
	for (size_t i = 0; i < count; i++) {
		writeReg(phy, table[i].offset, table[i].value);
	}
}

// ufs_qcom_phy_qmp_v4_power_control(true): clear the RX clock-edge select on both lanes, then
// bring the PHY out of analog power collapse.
void phyPowerControl(uintptr_t phy)
{
	writeReg(phy, SM_PHY_RX0_IF_MODE,
	         readReg(phy, SM_PHY_RX0_IF_MODE) & ~SM_PHY_RX_IF_MODE_CLOCK_EDGE_BIT);
	writeReg(phy, SM_PHY_RX1_IF_MODE,
	         readReg(phy, SM_PHY_RX1_IF_MODE) & ~SM_PHY_RX_IF_MODE_CLOCK_EDGE_BIT);
	writeReg(phy, SM_PHY_POWER_DOWN, 0x1);
	__asm__ volatile("dsb sy" ::: "memory");
}

// ufs_qcom_phy_qmp_v4_phy_calibrate: soft reset around the tables, then release it.
void phyCalibrate(uintptr_t phy)
{
	writeReg(phy, SM_PHY_SW_RESET, 0x01);
	writeTable(phy, kSmPhyRateA, sizeof(kSmPhyRateA) / sizeof(kSmPhyRateA[0]));
	writeTable(phy, kSmPhySecondLane, sizeof(kSmPhySecondLane) / sizeof(kSmPhySecondLane[0]));
	writeReg(phy, SM_PHY_SW_RESET, 0x00);
}

// Device-owned lines must not be written back over DMA results, and host writes must reach
// memory before the doorbell. The DMA buffer is a dedicated page-aligned allocation, so whole
// 64-byte lines are always covered.
void cacheClean(const void *start, size_t length)
{
	uintptr_t address = reinterpret_cast<uintptr_t>(start) & ~uintptr_t(63);
	for (uintptr_t end = reinterpret_cast<uintptr_t>(start) + length; address < end; address += 64) {
		__asm__ volatile("dc cvac, %0" :: "r"(address) : "memory");
	}
	__asm__ volatile("dsb sy" ::: "memory");
}

void cacheCleanInvalidate(const void *start, size_t length)
{
	uintptr_t address = reinterpret_cast<uintptr_t>(start) & ~uintptr_t(63);
	for (uintptr_t end = reinterpret_cast<uintptr_t>(start) + length; address < end; address += 64) {
		__asm__ volatile("dc civac, %0" :: "r"(address) : "memory");
	}
	__asm__ volatile("dsb sy" ::: "memory");
}

// Invalidate without writing back: a device-to-memory result must not be replaced by a dirty
// line the CPU still holds from before the transfer.
void cacheInvalidate(const void *start, size_t length)
{
	uintptr_t address = reinterpret_cast<uintptr_t>(start) & ~uintptr_t(63);
	for (uintptr_t end = reinterpret_cast<uintptr_t>(start) + length; address < end; address += 64) {
		__asm__ volatile("dc ivac, %0" :: "r"(address) : "memory");
	}
	__asm__ volatile("dsb sy" ::: "memory");
}

void zeroWords(void *start, size_t length)
{
	volatile uint32_t *word = static_cast<volatile uint32_t *>(start);
	for (size_t index = 0; index < length / 4; index++) {
		word[index] = 0;
	}
}

void putBe32(uint8_t *at, uint32_t value)
{
	at[0] = (uint8_t)(value >> 24);
	at[1] = (uint8_t)(value >> 16);
	at[2] = (uint8_t)(value >> 8);
	at[3] = (uint8_t)value;
}

uint32_t loadBe32(const uint8_t *at)
{
	return ((uint32_t)at[0] << 24) | ((uint32_t)at[1] << 16) | ((uint32_t)at[2] << 8) | at[3];
}

// Level 8: ring the doorbell from a helper thread. A store that never returns only wedges that
// thread; this CPU can still read the post-ring state and report it within milliseconds, before
// a stalled transfer takes the whole interconnect down (the level-7 runs died before the watchdog
// could print anything).
volatile uint32_t gRingDone;
uintptr_t gRingHc;

void ringThread(void *, wait_result_t)
{
	writeReg(gRingHc, SM_UFS_REG_UTRLDBR, 1);
	gRingDone = 1;
}

// Level 41 observers. The level-40 run proved both SMR entries read back, so the stream
// mapping is in place, and the ring still took the machine down: the completion square that
// is painted unconditionally after the ring never appeared. These flags separate the three
// remaining explanations without letting a wedged read reach the reporter: a watcher thread
// records the SMMU fault syndrome into RAM, a second thread rings and marks entry separately
// from return, and this CPU only ever reads plain memory.
volatile uint32_t gL41Entered, gL41Done, gL41Ticks, gL41Gfsr, gL41Syn0, gL41Syn1;
uintptr_t gL41Smmu;
uintptr_t gL46Resp;
uintptr_t gL46Utrd;
uintptr_t gCapVa;
uint8_t *gBuildVa;
uint64_t gDmaPhys;
uint8_t gSetresp[16] = {};
unsigned gSettries = 0;
bool gSetok = false;

void l41Watcher(void *, wait_result_t)
{
	for (unsigned i = 0; i < 400; i++) {
		if (gL41Smmu != 0) {
			gL41Gfsr = readReg(gL41Smmu, 0x48);
			gL41Syn0 = readReg(gL41Smmu, 0x50);
			gL41Syn1 = readReg(gL41Smmu, 0x54);
		}
		gL41Ticks = gL41Ticks + 1;
		IODelay(1000);
	}
}

void l41Ring(void *, wait_result_t)
{
	gL41Entered = 1;
	writeReg(gRingHc, SM_UFS_REG_UTRLDBR, 1);
	gL41Done = 1;
}

// Level 43 workers. The level 42 frame showed iteration 0 of the repaint loop and then a
// frozen strip, so the flags as seen on screen are always one repaint behind the freeze.
// These workers paint at the moment something happens instead: the ring thread greens the
// x=160 square immediately before the doorbell store (that paint must land if the thread
// ever ran) and the x=192 square only after the store retired; the watcher paints one
// strip square per 250 ms, so the strip length on a frozen frame is the survival time of
// the memory path, and the fault squares the moment the SMMU records a global fault.
// One 8x24 square per bit, green = 1, dark = 0. A frozen frame carries the last value
// each observer saw, which is the only channel that survives a wedged interconnect.
static void paintBits(uint32_t y, uint32_t x0, uint32_t value, unsigned count)
{
	for (unsigned b = 0; b < count; b++) {
		fbFill(gFb, x0 + b * 10, y, 8, 24, (value >> b) & 1 ? 0xff00ff00U : 0xff202020U);
	}
}

void l43Ring(void *, wait_result_t)
{
	gL41Entered = 1;
	fbFill(gFb, 160, 896, 24, 24, 0xff00ff00U);
	writeReg(gRingHc, SM_UFS_REG_UTRLDBR, 1);
	gL41Done = 1;
	fbFill(gFb, 192, 896, 24, 24, 0xff00ff00U);
}

// Level 45 observers. Level 44 froze with every square yellow past the ring, and its
// watcher interleaved SMMU and UFS reads, so which port dies first was unknowable. Two
// dedicated watchers now each touch ONE port and paint their own survival strip: A reads
// only the SMMU (strip y=944, gfsr/syn bit patterns y=1040), B reads only the UFS page
// (DBR latched/cleared + IS event squares and the IS bit pattern y=1088). The main thread
// paints its own strip at y=992 with pure memory writes. If the main strip keeps growing
// while A's stops, only MMIO died and the machine is alive; A's fault bits then name the
// SMMU fault and its stream ID, which decides between a wrong bypass mapping and XPU/NoC.
void l45WatcherA(void *, wait_result_t)
{
	bool faultPainted = false;
	fbFill(gFb, 384, 896, 24, 24, 0xff00ff00U);
	for (unsigned i = 0; i < 120000; i++) {
		if (gL41Smmu != 0) {
			gL41Gfsr = readReg(gL41Smmu, 0x48);
			gL41Syn1 = readReg(gL41Smmu, 0x54);
			paintBits(1040, 16, gL41Gfsr, 32);
			if (gL41Gfsr != 0 && !faultPainted) {
				faultPainted = true;
				uint32_t sid = gL41Syn1 & 0xfffU;
				fbFill(gFb, 256, 896, 24, 24, 0xff00ff00U);
				if (sid == 0x0e0U) {
					fbFill(gFb, 288, 896, 24, 24, 0xff00ff00U);
				} else if (sid == 0x4e0U) {
					fbFill(gFb, 320, 896, 24, 24, 0xff00ff00U);
				} else if (sid != 0) {
					fbFill(gFb, 352, 896, 24, 24, 0xff00ff00U);
				}
			}
		}
		if (i == 50) {
			fbFill(gFb, 224, 896, 24, 24, 0xff00ff00U);
		}
		if (i % 250 == 0 && i / 250 < 120) {
			fbFill(gFb, 16 + (i / 250) * 6, 944, 6, 24,
			       (i / 250) & 1 ? 0xffffffffU : 0xff00ff00U);
		}
		gL41Ticks = gL41Ticks + 1;
		IODelay(1000);
	}
}

void l45WatcherB(void *, wait_result_t)
{
	bool latchedPainted = false;
	bool clearedPainted = false;
	bool isPainted = false;
	for (unsigned i = 0; i < 120000; i++) {
		if (gRingHc != 0) {
			uint32_t dbr = readReg(gRingHc, SM_UFS_REG_UTRLDBR);
			uint32_t is = readReg(gRingHc, SM_UFS_REG_IS);
			gL41Syn0 = is;
			paintBits(1088, 16, is, 32);
			if ((dbr & 1) && !latchedPainted) {
				latchedPainted = true;
				fbFill(gFb, 416, 896, 24, 24, 0xff00ff00U);
			}
			if (!(dbr & 1) && !clearedPainted) {
				clearedPainted = true;
				fbFill(gFb, 448, 896, 24, 24, 0xff00ff00U);
			}
			if (is != 0 && !isPainted) {
				isPainted = true;
				fbFill(gFb, 480, 896, 24, 24, 0xff00ff00U);
			}
		}
		IODelay(1000);
	}
}

// Level 46 host watcher: the device writes the response UPIU through DMA, so the stale
// cache lines are dropped before the CPU looks at it. The response and the UTRD are then
// mirrored on screen as bit rows (8 bits per byte, green = 1), which makes the SCSI status
// and the sense data readable from a frozen frame. rsp[7] is the SCSI status byte the same
// way the vendor parse reads it: 2 = CHECK CONDITION, 0 = GOOD.
void l46WatcherB(void *, wait_result_t)
{
	bool latchedPainted = false;
	bool clearedPainted = false;
	bool isPainted = false;
	bool respPainted = false;
	bool ccPainted = false;
	bool goodPainted = false;
	for (unsigned i = 0; i < 120000; i++) {
		if (gRingHc != 0) {
			uint32_t dbr = readReg(gRingHc, SM_UFS_REG_UTRLDBR);
			uint32_t is = readReg(gRingHc, SM_UFS_REG_IS);
			gL41Syn0 = is;
			paintBits(1088, 16, is, 32);
			if ((dbr & 1) && !latchedPainted) {
				latchedPainted = true;
				fbFill(gFb, 416, 896, 24, 24, 0xff00ff00U);
			}
			if (!(dbr & 1) && !clearedPainted) {
				clearedPainted = true;
				fbFill(gFb, 448, 896, 24, 24, 0xff00ff00U);
			}
			if (is != 0 && !isPainted) {
				isPainted = true;
				fbFill(gFb, 480, 896, 24, 24, 0xff00ff00U);
			}
			const volatile uint8_t *rsp = reinterpret_cast<const volatile uint8_t *>(gL46Resp);
			const volatile uint8_t *ut = reinterpret_cast<const volatile uint8_t *>(gL46Utrd);
			if (!respPainted && rsp[0] != 0) {
				respPainted = true;
				fbFill(gFb, 512, 896, 24, 24, 0xff00ff00U);
			}
			if (!ccPainted && rsp[0] != 0 && rsp[7] == 0x02) {
				ccPainted = true;
				fbFill(gFb, 544, 896, 24, 24, 0xff00ff00U);
			}
			if (!goodPainted && rsp[0] != 0 && rsp[7] == 0x00) {
				goodPainted = true;
				fbFill(gFb, 576, 896, 24, 24, 0xff00ff00U);
			}
			if (i % 16 == 0) {
				for (unsigned byte = 0; byte < 16; byte++) {
					paintBits(1136 + (byte / 8) * 48, 16 + (byte % 8) * 84, ut[byte], 8);
				}
				for (unsigned byte = 0; byte < 64; byte++) {
					paintBits(1232 + (byte / 8) * 48, 16 + (byte % 8) * 84, rsp[byte], 8);
				}
			}
		}
		IODelay(1000);
	}
}

// Build one UTRD for the shared page layout: UTRD at the page start, UCD at +0x40,
// response UPIU at UCD+512, PRDT at UCD+1024, data at page+12288.
static void writeUtrd(uint8_t *page, uint64_t pagePhys, uint32_t dataLen, bool toHost)
{
	uint64_t ucdPhys = pagePhys + 0x40;
	*reinterpret_cast<uint32_t *>(page + 0) = 0x10000000U | ((dataLen ? (toHost ? 2U : 1U) : 0U) << 25);
	*reinterpret_cast<uint32_t *>(page + 8) = 0x0fU;
	*reinterpret_cast<uint32_t *>(page + 16) = (uint32_t)ucdPhys;
	*reinterpret_cast<uint32_t *>(page + 20) = (uint32_t)(ucdPhys >> 32);
	*reinterpret_cast<uint32_t *>(page + 24) = (512U >> 2) | ((512U >> 2) << 16);
	*reinterpret_cast<uint32_t *>(page + 28) = (dataLen ? 1U : 0U) | ((1024U >> 2) << 16);
	if (dataLen) {
		uint8_t *prd = page + 0x40 + 1024;
		uint64_t dataPhys = pagePhys + 12288;
		*reinterpret_cast<uint32_t *>(prd + 0) = (uint32_t)dataPhys;
		*reinterpret_cast<uint32_t *>(prd + 4) = (uint32_t)(dataPhys >> 32);
		*reinterpret_cast<uint32_t *>(prd + 8) = 0;
		*reinterpret_cast<uint32_t *>(prd + 12) = dataLen - 1;
	}
}

static bool pollDoorbellClear(uintptr_t hc)
{
	for (unsigned loop = 0; loop < 300; loop++) {
		if ((readReg(hc, SM_UFS_REG_UTRLDBR) & 1) == 0) {
			return true;
		}
		IODelay(1000);
	}
	return false;
}

// The fDeviceInit sequencer. SET FLAG (opcode 6, IDN 1) starts the device initialization,
// READ FLAG (opcode 5) then polls until the device self-clears it, TUR proves the READY
// state and READ CAPACITY(10) returns the geometry. Every step paints its own square and
// every poll is bounded, so a frozen frame shows exactly how far the sequence got.
void l47Worker(void *, wait_result_t)
{
	uint8_t *b = gBuildVa;
	uint8_t *ucd = b + 0x40;
	uintptr_t hc = gRingHc;
	const volatile uint8_t *rsp = reinterpret_cast<const volatile uint8_t *>(gL46Resp);

	gL41Entered = 1;
	fbFill(gFb, 160, 896, 24, 24, 0xff00ff00U);

	zeroWords(b, 32);
	zeroWords(ucd, 1040);
	ucd[0] = 0x16;   // QUERY REQUEST
	ucd[5] = 0x81;   // standard write request (0x81 per the ColorOS kernel, NOT 0x02 -
	                 // the 0x02 form was silently ignored, flag never set)
	ucd[12] = 0x06;  // opcode: SET FLAG
	ucd[13] = 0x01;  // IDN: fDeviceInit
	*reinterpret_cast<volatile uint8_t *>(ucd + 14) = 0;
	*reinterpret_cast<volatile uint8_t *>(ucd + 15) = 0;
	writeUtrd(b, gDmaPhys, 0, false);
	cacheClean(b, 16384);
	writeReg(hc, SM_UFS_REG_IS, 0xffffffffU);
	writeReg(hc, SM_UFS_REG_UTRLDBR, 1);
	if (pollDoorbellClear(hc)) {
		gL41Done = 1;
		fbFill(gFb, 192, 896, 24, 24, 0xff00ff00U);
		fbFill(gFb, 448, 896, 24, 24, 0xff00ff00U);
		if (rsp[0] != 0) {
			fbFill(gFb, 512, 896, 24, 24, 0xff00ff00U);
		}
		if (rsp[12] == 0x36 && rsp[13] == 0x06) {
			fbFill(gFb, 544, 896, 24, 24, 0xff00ff00U);
		}
	} else {
		fbFill(gFb, 448, 896, 24, 24, 0xffff0000U);
		for (;;) {
			IODelay(1000000);
		}
	}

	bool ready = false;
	for (unsigned attempt = 0; attempt < 100; attempt++) {
		zeroWords(b, 32);
		zeroWords(ucd, 1040);
		ucd[0] = 0x16;   // QUERY REQUEST
		ucd[5] = 0x01;   // standard read request
		ucd[12] = 0x05;  // opcode: READ FLAG
		ucd[13] = 0x01;  // IDN: fDeviceInit
		*reinterpret_cast<volatile uint8_t *>(ucd + 14) = 0;
		*reinterpret_cast<volatile uint8_t *>(ucd + 15) = 0;
		writeUtrd(b, gDmaPhys, 0, false);
		cacheClean(b, 16384);
		writeReg(hc, SM_UFS_REG_IS, 0xffffffffU);
		writeReg(hc, SM_UFS_REG_UTRLDBR, 1);
		if (!pollDoorbellClear(hc)) {
			fbFill(gFb, 576, 896, 24, 24, 0xffff0000U);
			break;
		}
		if (rsp[0] == 0x36 && rsp[18] == 0 && rsp[19] == 0 && rsp[20] == 0 && rsp[21] == 0) {
			ready = true;
			fbFill(gFb, 576, 896, 24, 24, 0xff00ff00U);
			break;
		}
		IODelay(50000);
	}

	zeroWords(b, 32);
	zeroWords(ucd, 1040);
	ucd[0] = 0x01;   // COMMAND UPIU
	ucd[1] = 0x00;   // no data phase
	ucd[2] = 0x00;   // LUN 0
	*reinterpret_cast<volatile uint8_t *>(ucd + 12) = 0;
	*reinterpret_cast<volatile uint8_t *>(ucd + 13) = 0;
	*reinterpret_cast<volatile uint8_t *>(ucd + 14) = 0;
	*reinterpret_cast<volatile uint8_t *>(ucd + 15) = 0;
	for (unsigned index = 0; index < 6; index++) {
		ucd[16 + index] = 0;   // TEST UNIT READY CDB
	}
	writeUtrd(b, gDmaPhys, 0, false);
	cacheClean(b, 16384);
	writeReg(hc, SM_UFS_REG_IS, 0xffffffffU);
	writeReg(hc, SM_UFS_REG_UTRLDBR, 1);
	if (pollDoorbellClear(hc)) {
		fbFill(gFb, 608, 896, 24, 24, 0xff00ff00U);
		if (rsp[0] != 0 && rsp[7] == 0x00) {
			fbFill(gFb, 640, 896, 24, 24, 0xff00ff00U);
		}
	} else {
		fbFill(gFb, 608, 896, 24, 24, 0xffff0000U);
	}

	if (ready) {
		zeroWords(b, 32);
		zeroWords(ucd, 1040);
		ucd[0] = 0x01;   // COMMAND UPIU
		ucd[1] = 0x40;   // flags: read
		ucd[2] = 0x00;   // LUN 0
		*reinterpret_cast<volatile uint8_t *>(ucd + 12) = 0;
		*reinterpret_cast<volatile uint8_t *>(ucd + 13) = 0;
		*reinterpret_cast<volatile uint8_t *>(ucd + 14) = 0;
		*reinterpret_cast<volatile uint8_t *>(ucd + 15) = 8;
		for (unsigned index = 0; index < 10; index++) {
			ucd[16 + index] = 0;   // READ CAPACITY(10) CDB
		}
		ucd[17] = 0x25;  // SERVICE ACTION / opcode: READ CAPACITY(10)
		writeUtrd(b, gDmaPhys, 8, true);
		cacheClean(b, 16384);
		writeReg(hc, SM_UFS_REG_IS, 0xffffffffU);
		writeReg(hc, SM_UFS_REG_UTRLDBR, 1);
		if (pollDoorbellClear(hc)) {
			fbFill(gFb, 672, 896, 24, 24, 0xff00ff00U);
			const volatile uint8_t *cap = reinterpret_cast<const volatile uint8_t *>(gCapVa);
			for (unsigned byte = 0; byte < 8; byte++) {
				paintBits(1712, 16 + byte * 84, cap[byte], 8);
			}
		} else {
			fbFill(gFb, 672, 896, 24, 24, 0xffff0000U);
		}
	}
	for (;;) {
		IODelay(1000000);
	}
}

// Level 48 fixes the one bug level 47 exposed: the READ-FLAG value offset was guessed, so
// "fDeviceInit done" fired on reserved zero bytes and the TUR ran while the device was
// still initializing (GOOD stayed yellow). The readiness poll is now the TUR itself - the
// device answers CHECK CONDITION (rsp[7]=2) until fDeviceInit completes, then GOOD (0).
// rsp[7] is the same byte the level-46 CHECK CONDITION detection already proved readable.
void l48Worker(void *, wait_result_t)
{
	uint8_t *b = gBuildVa;
	uint8_t *ucd = b + 0x40;
	uintptr_t hc = gRingHc;
	const volatile uint8_t *rsp = reinterpret_cast<const volatile uint8_t *>(gL46Resp);

	gL41Entered = 1;
	fbFill(gFb, 160, 896, 24, 24, 0xff00ff00U);

	zeroWords(b, 32);
	zeroWords(ucd, 1040);
	ucd[0] = 0x16;   // QUERY REQUEST
	ucd[5] = 0x81;   // standard write request (0x81 per the ColorOS kernel, NOT 0x02 -
	                 // the 0x02 form was silently ignored, flag never set)
	ucd[12] = 0x06;  // opcode: SET FLAG
	ucd[13] = 0x01;  // IDN: fDeviceInit
	*reinterpret_cast<volatile uint8_t *>(ucd + 14) = 0;
	*reinterpret_cast<volatile uint8_t *>(ucd + 15) = 0;
	writeUtrd(b, gDmaPhys, 0, false);
	cacheClean(b, 16384);
	writeReg(hc, SM_UFS_REG_IS, 0xffffffffU);
	writeReg(hc, SM_UFS_REG_UTRLDBR, 1);
	if (pollDoorbellClear(hc)) {
		gL41Done = 1;
		fbFill(gFb, 192, 896, 24, 24, 0xff00ff00U);
		fbFill(gFb, 448, 896, 24, 24, 0xff00ff00U);
		if (rsp[0] != 0) {
			fbFill(gFb, 512, 896, 24, 24, 0xff00ff00U);
		}
		if (rsp[12] == 0x36 && rsp[13] == 0x06) {
			fbFill(gFb, 544, 896, 24, 24, 0xff00ff00U);
		}
		fbFill(gFb, 576, 896, 24, 24, 0xff00ff00U);
	} else {
		fbFill(gFb, 448, 896, 24, 24, 0xffff0000U);
		for (;;) {
			IODelay(1000000);
		}
	}

	bool ready = false;
	for (unsigned attempt = 0; attempt < (gSmUfs.level == 56 ? 150 : (gSmUfs.level == 49 ? 300 : 60)); attempt++) {
		zeroWords(b, 32);
		zeroWords(ucd, 1040);
		ucd[0] = 0x01;   // COMMAND UPIU
		ucd[1] = 0x00;   // no data phase
		ucd[2] = 0x00;   // LUN 0
		*reinterpret_cast<volatile uint8_t *>(ucd + 12) = 0;
		*reinterpret_cast<volatile uint8_t *>(ucd + 13) = 0;
		*reinterpret_cast<volatile uint8_t *>(ucd + 14) = 0;
		*reinterpret_cast<volatile uint8_t *>(ucd + 15) = 0;
		for (unsigned index = 0; index < 6; index++) {
			ucd[16 + index] = 0;   // TEST UNIT READY CDB
		}
		writeUtrd(b, gDmaPhys, 0, false);
		cacheClean(b, 16384);
		writeReg(hc, SM_UFS_REG_IS, 0xffffffffU);
		writeReg(hc, SM_UFS_REG_UTRLDBR, 1);
		if (!pollDoorbellClear(hc)) {
			fbFill(gFb, 608, 896, 24, 24, 0xffff0000U);
			break;
		}
		fbFill(gFb, 608, 896, 24, 24, 0xff00ff00U);
		if (rsp[0] != 0 && rsp[7] == 0x00) {
			ready = true;
			fbFill(gFb, 640, 896, 24, 24, 0xff00ff00U);
			break;
		}
		for (unsigned byte = 0; byte < 64; byte++) {
			paintBits(1232 + (byte / 8) * 48, 16 + (byte % 8) * 84, rsp[byte], 8);
		}
		paintBits(1664, 16, rsp[7], 8);
		if (attempt < 120) {
			fbFill(gFb, 16 + attempt * 6, 1616, 6, 24,
			       (attempt & 1) ? 0xffffffffU : 0xff00ff00U);
		}
		IODelay(100000);
	}

	if (ready) {
		zeroWords(b, 32);
		zeroWords(ucd, 1040);
		ucd[0] = 0x01;   // COMMAND UPIU
		ucd[1] = 0x40;   // flags: read
		ucd[2] = 0x00;   // LUN 0
		*reinterpret_cast<volatile uint8_t *>(ucd + 12) = 0;
		*reinterpret_cast<volatile uint8_t *>(ucd + 13) = 0;
		*reinterpret_cast<volatile uint8_t *>(ucd + 14) = 0;
		*reinterpret_cast<volatile uint8_t *>(ucd + 15) = 8;
		for (unsigned index = 0; index < 10; index++) {
			ucd[16 + index] = 0;   // READ CAPACITY(10) CDB
		}
		ucd[17] = 0x25;  // READ CAPACITY(10)
		writeUtrd(b, gDmaPhys, 8, true);
		cacheClean(b, 16384);
		writeReg(hc, SM_UFS_REG_IS, 0xffffffffU);
		writeReg(hc, SM_UFS_REG_UTRLDBR, 1);
		if (pollDoorbellClear(hc)) {
			fbFill(gFb, 672, 896, 24, 24, 0xff00ff00U);
			const volatile uint8_t *cap = reinterpret_cast<const volatile uint8_t *>(gCapVa);
			for (unsigned byte = 0; byte < 8; byte++) {
				paintBits(1712, 16 + byte * 84, cap[byte], 8);
			}
		} else {
			fbFill(gFb, 672, 896, 24, 24, 0xffff0000U);
		}
	}
	for (;;) {
		IODelay(1000000);
	}
}

// Level 50 decoded on hardware: the SET FLAG response is a proper query response
// (36 00 00 00 00 02 fe 00 | 00 00 00 00 06 01 00 00) - fDeviceInit accepted - while the
// TUR answers 21 .. 01 02 (CHECK CONDITION) with a 20-byte sense segment that the 16-byte
// dump missed. Level 51 widens the dump: SET FLAG response, READ FLAG response (the flag
// value per spec lives at bytes 18-21) and the TUR response with 48 bytes so the sense is
// inside the picture no matter which offset the segment starts at.
static uint32_t pack32(const volatile uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
void l50Worker(void *, wait_result_t)
{
	uint8_t *b = gBuildVa;
	uint8_t *ucd = b + 0x40;
	uintptr_t hc = gRingHc;
	const volatile uint8_t *rsp = reinterpret_cast<const volatile uint8_t *>(gL46Resp);
	uint8_t sf[16] = {};

	gL41Entered = 1;
	fbFill(gFb, 160, 896, 24, 24, 0xff00ff00U);

	zeroWords(b, 32);
	zeroWords(ucd, 1040);
	ucd[0] = 0x16;   // QUERY REQUEST
	ucd[5] = 0x81;   // standard write request (0x81 per the ColorOS kernel, NOT 0x02 -
	                 // the 0x02 form was silently ignored, flag never set)
	ucd[12] = 0x06;  // opcode: SET FLAG
	ucd[13] = 0x01;  // IDN: fDeviceInit
	*reinterpret_cast<volatile uint8_t *>(ucd + 14) = 0;
	*reinterpret_cast<volatile uint8_t *>(ucd + 15) = 0;
	writeUtrd(b, gDmaPhys, 0, false);
	cacheClean(b, 16384);
	writeReg(hc, SM_UFS_REG_IS, 0xffffffffU);
	writeReg(hc, SM_UFS_REG_UTRLDBR, 1);
	if (!pollDoorbellClear(hc)) {
		panic("SMUFS l50 setflag: doorbell never cleared");
	}
	gL41Done = 1;
	fbFill(gFb, 192, 896, 24, 24, 0xff00ff00U);
	fbFill(gFb, 448, 896, 24, 24, 0xff00ff00U);
	for (unsigned i = 0; i < 16; i++) {
		sf[i] = rsp[i];
	}

	zeroWords(b, 32);
	zeroWords(ucd, 1040);
	ucd[0] = 0x01;   // COMMAND UPIU
	ucd[1] = 0x00;   // no data phase
	ucd[2] = 0x00;   // LUN 0
	for (unsigned index = 0; index < 6; index++) {
		ucd[16 + index] = 0;   // TEST UNIT READY CDB
	}
	writeUtrd(b, gDmaPhys, 0, false);
	cacheClean(b, 16384);
	writeReg(hc, SM_UFS_REG_IS, 0xffffffffU);
	writeReg(hc, SM_UFS_REG_UTRLDBR, 1);
	if (!pollDoorbellClear(hc)) {
		panic("SMUFS l50 tur: doorbell never cleared");
	}
	fbFill(gFb, 608, 896, 24, 24, 0xff00ff00U);

	const volatile uint8_t *ut = reinterpret_cast<const volatile uint8_t *>(gL46Utrd);
	panic("SMUFS l50\n"
	      "sf  %02x %02x %02x %02x %02x %02x %02x %02x\n"
	      "sf+ %02x %02x %02x %02x %02x %02x %02x %02x\n"
	      "tur %02x %02x %02x %02x %02x %02x %02x %02x\n"
	      "tur+%02x %02x %02x %02x %02x %02x %02x %02x\n"
	      "se  %02x %02x %02x %02x %02x %02x %02x %02x\n"
	      "u   %02x %02x %02x %02x",
	      sf[0], sf[1], sf[2], sf[3], sf[4], sf[5], sf[6], sf[7],
	      sf[8], sf[9], sf[10], sf[11], sf[12], sf[13], sf[14], sf[15],
	      rsp[0], rsp[1], rsp[2], rsp[3], rsp[4], rsp[5], rsp[6], rsp[7],
	      rsp[8], rsp[9], rsp[10], rsp[11], rsp[12], rsp[13], rsp[14], rsp[15],
	      rsp[16], rsp[17], rsp[18], rsp[19], rsp[20], rsp[21], rsp[22], rsp[23],
	      ut[8], ut[9], ut[10], ut[11]);
	for (;;) {
		IODelay(1000000);
	}
}

// Level 51: the full-width dump. Three commands (SET FLAG fDeviceInit, READ FLAG
// fDeviceInit, TEST UNIT READY), each response snapshotted, then one panic prints
// everything: sf/rf/tur rows of 8 bytes plus the extended areas where the flag value and
// the sense data live per the UFS spec.
void l51Worker(void *, wait_result_t)
{
	uint8_t *b = gBuildVa;
	uint8_t *ucd = b + 0x40;
	uintptr_t hc = gRingHc;
	const volatile uint8_t *rsp = reinterpret_cast<const volatile uint8_t *>(gL46Resp);
	uint8_t sf[32] = {};
	uint8_t rf[32] = {};
	uint8_t tur[48] = {};

	gL41Entered = 1;
	fbFill(gFb, 160, 896, 24, 24, 0xff00ff00U);

	zeroWords(b, 32);
	zeroWords(ucd, 1040);
	ucd[0] = 0x16;   // QUERY REQUEST
	ucd[5] = 0x81;   // standard write request (0x81 per the ColorOS kernel, NOT 0x02 -
	                 // the 0x02 form was silently ignored, flag never set)
	ucd[12] = 0x06;  // opcode: SET FLAG
	ucd[13] = 0x01;  // IDN: fDeviceInit
	writeUtrd(b, gDmaPhys, 0, false);
	cacheClean(b, 16384);
	writeReg(hc, SM_UFS_REG_IS, 0xffffffffU);
	writeReg(hc, SM_UFS_REG_UTRLDBR, 1);
	if (!pollDoorbellClear(hc)) {
		panic("SMUFS l51 setflag: doorbell never cleared");
	}
	for (unsigned i = 0; i < 32; i++) {
		sf[i] = rsp[i];
	}

	zeroWords(b, 32);
	zeroWords(ucd, 1040);
	ucd[0] = 0x16;   // QUERY REQUEST
	ucd[5] = 0x01;   // standard read request
	ucd[12] = 0x05;  // opcode: READ FLAG
	ucd[13] = 0x01;  // IDN: fDeviceInit
	writeUtrd(b, gDmaPhys, 0, false);
	cacheClean(b, 16384);
	writeReg(hc, SM_UFS_REG_IS, 0xffffffffU);
	writeReg(hc, SM_UFS_REG_UTRLDBR, 1);
	if (!pollDoorbellClear(hc)) {
		panic("SMUFS l51 readflag: doorbell never cleared");
	}
	for (unsigned i = 0; i < 32; i++) {
		rf[i] = rsp[i];
	}

	zeroWords(b, 32);
	zeroWords(ucd, 1040);
	ucd[0] = 0x01;   // COMMAND UPIU
	ucd[1] = 0x00;   // no data phase
	ucd[2] = 0x00;   // LUN 0
	for (unsigned index = 0; index < 6; index++) {
		ucd[16 + index] = 0;   // TEST UNIT READY CDB
	}
	writeUtrd(b, gDmaPhys, 0, false);
	cacheClean(b, 16384);
	writeReg(hc, SM_UFS_REG_IS, 0xffffffffU);
	writeReg(hc, SM_UFS_REG_UTRLDBR, 1);
	if (!pollDoorbellClear(hc)) {
		panic("SMUFS l51 tur: doorbell never cleared");
	}
	for (unsigned i = 0; i < 48; i++) {
		tur[i] = rsp[i];
	}

	const volatile uint8_t *ut = reinterpret_cast<const volatile uint8_t *>(gL46Utrd);
	panic("SMUFS l51\n"
	      "sf  %08x %08x %08x %08x\n"
	      "rf  %08x %08x %08x %08x\n"
	      "tur %08x %08x %08x\n"
	      "tur+%08x %08x %08x %08x %08x %08x\n"
	      "se  %08x %08x %08x\n"
	      "u   %08x",
	      pack32(sf + 0), pack32(sf + 4), pack32(sf + 8), pack32(sf + 12),
	      pack32(rf + 0), pack32(rf + 4), pack32(rf + 8), pack32(rf + 12),
	      pack32(tur + 0), pack32(tur + 4), pack32(tur + 8),
	      pack32(tur + 12), pack32(tur + 16), pack32(tur + 20),
	      pack32(tur + 24), pack32(tur + 28), pack32(tur + 32),
	      pack32(tur + 36), pack32(tur + 40), pack32(tur + 44),
	      pack32(ut + 8));
	for (;;) {
		IODelay(1000000);
	}
}

// Level 52: the Linux algorithm. The 51 dump proved SET FLAG is accepted and the TUR
// sense is key 2 / ASC 04 / ASCQ 02 (initializing required) right after the flag is set.
// The device's own readiness signal is the fDeviceInit flag VALUE (query response bytes
// 18-21 per the spec): 1 while initializing, 0 when done. Poll READ FLAG every 200 ms
// for 60 s, then TUR, then capacity. The panic at the end prints the last responses.
void l52Worker(void *, wait_result_t)
{
	uint8_t *b = gBuildVa;
	uint8_t *ucd = b + 0x40;
	uintptr_t hc = gRingHc;
	const volatile uint8_t *rsp = reinterpret_cast<const volatile uint8_t *>(gL46Resp);
	uint8_t sf[32] = {};
	uint8_t rf[32] = {};
	uint8_t tur[48] = {};

	gL41Entered = 1;
	fbFill(gFb, 160, 896, 24, 24, 0xff00ff00U);

	zeroWords(b, 32);
	zeroWords(ucd, 1040);
	ucd[0] = 0x16;   // QUERY REQUEST
	ucd[5] = 0x81;   // standard write request (0x81 per the ColorOS kernel, NOT 0x02 -
	                 // the 0x02 form was silently ignored, flag never set)
	ucd[12] = 0x06;  // opcode: SET FLAG
	ucd[13] = 0x01;  // IDN: fDeviceInit
	writeUtrd(b, gDmaPhys, 0, false);
	cacheClean(b, 16384);
	writeReg(hc, SM_UFS_REG_IS, 0xffffffffU);
	writeReg(hc, SM_UFS_REG_UTRLDBR, 1);
	if (!pollDoorbellClear(hc)) {
		panic("SMUFS l52 setflag: doorbell never cleared");
	}
	for (unsigned i = 0; i < 32; i++) {
		sf[i] = rsp[i];
	}

	bool sawOne = false;
	bool done = false;
	unsigned attempts = 0;
	for (unsigned attempt = 0; attempt < 300; attempt++) {
		attempts = attempt + 1;
		zeroWords(b, 32);
		zeroWords(ucd, 1040);
		ucd[0] = 0x16;   // QUERY REQUEST
		ucd[5] = 0x01;   // standard read request
		ucd[12] = 0x05;  // opcode: READ FLAG
		ucd[13] = 0x01;  // IDN: fDeviceInit
		writeUtrd(b, gDmaPhys, 0, false);
		cacheClean(b, 16384);
		writeReg(hc, SM_UFS_REG_IS, 0xffffffffU);
		writeReg(hc, SM_UFS_REG_UTRLDBR, 1);
		if (!pollDoorbellClear(hc)) {
			break;
		}
		for (unsigned i = 0; i < 32; i++) {
			rf[i] = rsp[i];
		}
		uint32_t value = ((uint32_t)rf[18] << 24) | ((uint32_t)rf[19] << 16) |
		                 ((uint32_t)rf[20] << 8) | rf[21];
		if (value == 1) {
			sawOne = true;
			fbFill(gFb, 576, 896, 24, 24, 0xff00ff00U);
		}
		if (value == 0 && sawOne) {
			done = true;
			break;
		}
		if (attempt < 120) {
			fbFill(gFb, 16 + attempt * 6, 1616, 6, 24,
			       (attempt & 1) ? 0xffffffffU : 0xff00ff00U);
		}
		IODelay(200000);
	}
	if (done) {
		fbFill(gFb, 576, 896, 24, 24, 0xff00ff00U);
	}

	bool good = false;
	if (done) {
		zeroWords(b, 32);
		zeroWords(ucd, 1040);
		ucd[0] = 0x01;   // COMMAND UPIU
		ucd[1] = 0x00;   // no data phase
		ucd[2] = 0x00;   // LUN 0
		for (unsigned index = 0; index < 6; index++) {
			ucd[16 + index] = 0;   // TEST UNIT READY CDB
		}
		writeUtrd(b, gDmaPhys, 0, false);
		cacheClean(b, 16384);
		writeReg(hc, SM_UFS_REG_IS, 0xffffffffU);
		writeReg(hc, SM_UFS_REG_UTRLDBR, 1);
		if (pollDoorbellClear(hc)) {
			for (unsigned i = 0; i < 48; i++) {
				tur[i] = rsp[i];
			}
			fbFill(gFb, 608, 896, 24, 24, 0xff00ff00U);
			if (rsp[0] != 0 && rsp[7] == 0x00) {
				good = true;
				fbFill(gFb, 640, 896, 24, 24, 0xff00ff00U);
			}
		}
	}

	uint32_t cap0 = 0, cap1 = 0;
	{   // capacity gate removed: try READ CAPACITY regardless of TUR
		zeroWords(b, 32);
		zeroWords(ucd, 1040);
		ucd[0] = 0x01;   // COMMAND UPIU
		ucd[1] = 0x40;   // flags: read
		ucd[2] = 0x00;   // LUN 0
		for (unsigned index = 0; index < 10; index++) {
			ucd[16 + index] = 0;   // READ CAPACITY(10) CDB
		}
		ucd[17] = 0x25;  // READ CAPACITY(10)
		writeUtrd(b, gDmaPhys, 8, true);
		cacheClean(b, 16384);
		writeReg(hc, SM_UFS_REG_IS, 0xffffffffU);
		writeReg(hc, SM_UFS_REG_UTRLDBR, 1);
		if (pollDoorbellClear(hc)) {
			fbFill(gFb, 672, 896, 24, 24, 0xff00ff00U);
			const volatile uint8_t *cap = reinterpret_cast<const volatile uint8_t *>(gCapVa);
			cap0 = pack32(cap + 0);
			cap1 = pack32(cap + 4);
		}
	}
	panic("SMUFS l52\n"
	      "sf  %08x %08x %08x %08x\n"
	      "rf  %08x %08x %08x %08x\n"
	      "rf+ %08x %08x %08x %08x\n"
	      "tur %08x %08x %08x\n"
	      "tur+%08x %08x %08x %08x %08x %08x\n"
	      "se  %08x %08x %08x\n"
	      "att %u done %u sawone %u good %u cap %08x %08x",
	      pack32(sf + 0), pack32(sf + 4), pack32(sf + 8), pack32(sf + 12),
	      pack32(rf + 0), pack32(rf + 4), pack32(rf + 8), pack32(rf + 12),
	      pack32(rf + 16), pack32(rf + 20), pack32(rf + 24), pack32(rf + 28),
	      pack32(tur + 0), pack32(tur + 4), pack32(tur + 8),
	      pack32(tur + 12), pack32(tur + 16), pack32(tur + 20),
	      pack32(tur + 24), pack32(tur + 28), pack32(tur + 32),
	      pack32(tur + 36), pack32(tur + 40), pack32(tur + 44),
	      attempts, done ? 1u : 0u, sawOne ? 1u : 0u, good ? 1u : 0u, cap0, cap1);
	for (;;) {
		IODelay(1000000);
	}
}

// Level 53: the level-52 dump never reached the camera - the kernel's own root-mount
// panic (no root device published yet) wipes the screen long before the 60 s readiness
// poll finishes. This worker panics ~1.5 s after boot with the two responses that decide
// everything: the SET FLAG query result and the FIRST READ FLAG value (bytes 18-21).
// value 1 = the device accepted fDeviceInit and is initializing (just extend the window);
// value 0 = the SET FLAG never took effect (the flag path itself is wrong).
void l53Worker(void *, wait_result_t)
{
	uint8_t *b = gBuildVa;
	uint8_t *ucd = b + 0x40;
	uintptr_t hc = gRingHc;
	const volatile uint8_t *rsp = reinterpret_cast<const volatile uint8_t *>(gL46Resp);
	uint8_t sf[32] = {};
	uint8_t rf[32] = {};

	gL41Entered = 1;
	fbFill(gFb, 160, 896, 24, 24, 0xff00ff00U);

	zeroWords(b, 32);
	zeroWords(ucd, 1040);
	ucd[0] = 0x16;   // QUERY REQUEST
	ucd[5] = 0x81;   // standard write request (0x81 per the ColorOS kernel, NOT 0x02 -
	                 // the 0x02 form was silently ignored, flag never set)
	ucd[12] = 0x06;  // opcode: SET FLAG
	ucd[13] = 0x01;  // IDN: fDeviceInit
	writeUtrd(b, gDmaPhys, 0, false);
	cacheClean(b, 16384);
	writeReg(hc, SM_UFS_REG_IS, 0xffffffffU);
	writeReg(hc, SM_UFS_REG_UTRLDBR, 1);
	if (!pollDoorbellClear(hc)) {
		panic("SMUFS l53 setflag: doorbell never cleared");
	}
	for (unsigned i = 0; i < 32; i++) {
		sf[i] = rsp[i];
	}

	zeroWords(b, 32);
	zeroWords(ucd, 1040);
	ucd[0] = 0x16;   // QUERY REQUEST
	ucd[5] = 0x01;   // standard read request
	ucd[12] = 0x05;  // opcode: READ FLAG
	ucd[13] = 0x01;  // IDN: fDeviceInit
	writeUtrd(b, gDmaPhys, 0, false);
	cacheClean(b, 16384);
	writeReg(hc, SM_UFS_REG_IS, 0xffffffffU);
	writeReg(hc, SM_UFS_REG_UTRLDBR, 1);
	if (!pollDoorbellClear(hc)) {
		panic("SMUFS l53 readflag: doorbell never cleared");
	}
	for (unsigned i = 0; i < 32; i++) {
		rf[i] = rsp[i];
	}

	panic("SMUFS l53\n"
	      "sf  %08x %08x %08x %08x %08x %08x %08x %08x\n"
	      "rf  %08x %08x %08x %08x %08x %08x %08x %08x\n"
	      "val %02x %02x %02x %02x",
	      pack32(sf + 0), pack32(sf + 4), pack32(sf + 8), pack32(sf + 12),
	      pack32(sf + 16), pack32(sf + 20), pack32(sf + 24), pack32(sf + 28),
	      pack32(rf + 0), pack32(rf + 4), pack32(rf + 8), pack32(rf + 12),
	      pack32(rf + 16), pack32(rf + 20), pack32(rf + 24), pack32(rf + 28),
	      rf[18], rf[19], rf[20], rf[21]);
	for (;;) {
		IODelay(1000000);
	}
}

// Level 54: the fDeviceInit value timeline. The 53 dump proved the SET FLAG query is
// accepted (36.. / fe 00 / 06 01 echo) while READ FLAG returns value 0 (bytes 20-23 per
// the ColorOS oufs kernel's utp_upiu_query: opcode 12, idn 13, index 14, selector 15,
// reserved_osf 16-17, length 18-19, VALUE 20-23). This worker SETs the flag once and
// then READs it 200 times at ~1 ms, painting each value as one square (green = 1). A run
// of green means the device started initializing late; all dark means the SET FLAG is
// being silently ignored. The panic prints the 200 values packed into 32-bit words.
void l54Worker(void *, wait_result_t)
{
	uint8_t *b = gBuildVa;
	uint8_t *ucd = b + 0x40;
	uintptr_t hc = gRingHc;
	const volatile uint8_t *rsp = reinterpret_cast<const volatile uint8_t *>(gL46Resp);
	uint32_t values[7] = {};

	gL41Entered = 1;
	fbFill(gFb, 160, 896, 24, 24, 0xff00ff00U);

	zeroWords(b, 32);
	zeroWords(ucd, 1040);
	ucd[0] = 0x16;   // QUERY REQUEST
	ucd[5] = 0x81;   // standard write request (0x81 per the ColorOS kernel, NOT 0x02 -
	                 // the 0x02 form was silently ignored, flag never set)
	ucd[12] = 0x06;  // opcode: SET FLAG
	ucd[13] = 0x01;  // IDN: fDeviceInit
	writeUtrd(b, gDmaPhys, 0, false);
	cacheClean(b, 16384);
	writeReg(hc, SM_UFS_REG_IS, 0xffffffffU);
	writeReg(hc, SM_UFS_REG_UTRLDBR, 1);
	if (!pollDoorbellClear(hc)) {
		panic("SMUFS l54 setflag: doorbell never cleared");
	}
	fbFill(gFb, 192, 896, 24, 24, 0xff00ff00U);
	fbFill(gFb, 448, 896, 24, 24, 0xff00ff00U);

	for (unsigned i = 0; i < 200; i++) {
		zeroWords(b, 32);
		zeroWords(ucd, 1040);
		ucd[0] = 0x16;   // QUERY REQUEST
		ucd[5] = 0x01;   // standard read request
		ucd[12] = 0x05;  // opcode: READ FLAG
		ucd[13] = 0x01;  // IDN: fDeviceInit
		writeUtrd(b, gDmaPhys, 0, false);
		cacheClean(b, 16384);
		writeReg(hc, SM_UFS_REG_IS, 0xffffffffU);
		writeReg(hc, SM_UFS_REG_UTRLDBR, 1);
		if (!pollDoorbellClear(hc)) {
			panic("SMUFS l54 readflag %u: doorbell never cleared", i);
		}
		uint32_t bit = (rsp[20] & 0x1) ? 1u : 0u;
		values[i / 32] |= bit << (i % 32);
		fbFill(gFb, 16 + i * 3, 1616, 3, 24, bit ? 0xff00ff00U : 0xff202020U);
	}

	zeroWords(b, 32);
	zeroWords(ucd, 1040);
	ucd[0] = 0x01;   // COMMAND UPIU
	ucd[1] = 0x00;   // no data phase
	ucd[2] = 0x00;   // LUN 0
	for (unsigned index = 0; index < 6; index++) {
		ucd[16 + index] = 0;   // TEST UNIT READY CDB
	}
	writeUtrd(b, gDmaPhys, 0, false);
	cacheClean(b, 16384);
	writeReg(hc, SM_UFS_REG_IS, 0xffffffffU);
	writeReg(hc, SM_UFS_REG_UTRLDBR, 1);
	bool turCleared = pollDoorbellClear(hc);
	uint8_t tur[16] = {};
	if (turCleared) {
		for (unsigned i = 0; i < 16; i++) {
			tur[i] = rsp[i];
		}
	}

	panic("SMUFS l54\n"
	      "v  %08x %08x %08x %08x\n"
	      "v+ %08x %08x %08x\n"
	      "tur %08x %08x %08x %08x",
	      values[0], values[1], values[2], values[3],
	      values[4], values[5], values[6],
	      pack32(tur + 0), pack32(tur + 4), pack32(tur + 8), pack32(tur + 12));
	for (;;) {
		IODelay(1000000);
	}
}

// Level 57: back-to-back TURs with no sleeps - thousands of probes in the first 10 s,
// capacity the moment GOOD, then a panic that always lands before the root-mount wipe.
void l57Worker(void *, wait_result_t)
{
	uint8_t *b = gBuildVa;
	uint8_t *ucd = b + 0x40;
	uintptr_t hc = gRingHc;
	const volatile uint8_t *rsp = reinterpret_cast<const volatile uint8_t *>(gL46Resp);
	uint8_t tur[48] = {};
	unsigned good = 0;

	gL41Entered = 1;
	fbFill(gFb, 160, 896, 24, 24, 0xff00ff00U);

	zeroWords(b, 32);
	zeroWords(ucd, 1040);
	ucd[0] = 0x16;   // QUERY REQUEST
	ucd[5] = 0x81;   // standard write request (0x81, NOT 0x02)
	ucd[12] = 0x06;  // opcode: SET FLAG
	ucd[13] = 0x01;  // IDN: fDeviceInit
	writeUtrd(b, gDmaPhys, 0, false);
	cacheClean(b, 16384);
	writeReg(hc, SM_UFS_REG_IS, 0xffffffffU);
	writeReg(hc, SM_UFS_REG_UTRLDBR, 1);
	if (!pollDoorbellClear(hc)) {
		panic("SMUFS l57 setflag: doorbell never cleared");
	}
	fbFill(gFb, 192, 896, 24, 24, 0xff00ff00U);
	fbFill(gFb, 448, 896, 24, 24, 0xff00ff00U);

	for (unsigned attempt = 0; attempt < 6000; attempt++) {
		zeroWords(b, 32);
		zeroWords(ucd, 1040);
		ucd[0] = 0x01;   // COMMAND UPIU
		ucd[1] = 0x00;   // no data phase
		ucd[2] = 0x00;   // LUN 0
		for (unsigned index = 0; index < 6; index++) {
			ucd[16 + index] = 0;   // TEST UNIT READY CDB
		}
		writeUtrd(b, gDmaPhys, 0, false);
		cacheClean(b, 16384);
		writeReg(hc, SM_UFS_REG_IS, 0xffffffffU);
		writeReg(hc, SM_UFS_REG_UTRLDBR, 1);
		if (!pollDoorbellClear(hc)) {
			break;
		}
		if (rsp[0] != 0 && rsp[7] == 0x00) {
			good = 1;
			for (unsigned i = 0; i < 48; i++) {
				tur[i] = rsp[i];
			}
			fbFill(gFb, 640, 896, 24, 24, 0xff00ff00U);
			break;
		}
		if (attempt < 120 && (attempt & 1)) {
			fbFill(gFb, 16 + (attempt >> 1) * 6, 1616, 6, 24, 0xff00ff00U);
		}
	}

	uint32_t cap0 = 0, cap1 = 0;
	unsigned capok = 0;
	{   // capacity gate removed: try READ CAPACITY regardless of TUR
		zeroWords(b, 32);
		zeroWords(ucd, 1040);
		ucd[0] = 0x01;   // COMMAND UPIU
		ucd[1] = 0x40;   // flags: read
		ucd[2] = 0x00;   // LUN 0
		for (unsigned index = 0; index < 10; index++) {
			ucd[16 + index] = 0;   // READ CAPACITY(10) CDB
		}
		ucd[17] = 0x25;  // READ CAPACITY(10)
		writeUtrd(b, gDmaPhys, 8, true);
		cacheClean(b, 16384);
		writeReg(hc, SM_UFS_REG_IS, 0xffffffffU);
		writeReg(hc, SM_UFS_REG_UTRLDBR, 1);
		if (pollDoorbellClear(hc)) {
			capok = 1;
			fbFill(gFb, 672, 896, 24, 24, 0xff00ff00U);
			const volatile uint8_t *cap = reinterpret_cast<const volatile uint8_t *>(gCapVa);
			cap0 = pack32(cap + 0);
			cap1 = pack32(cap + 4);
		}
	}
	panic("SMUFS l57\n"
	      "good %u capok %u\n"
	      "tur %08x %08x %08x %08x\n"
	      "tur+%08x %08x %08x %08x\n"
	      "cap %08x %08x",
	      good, capok,
	      pack32(tur + 0), pack32(tur + 4), pack32(tur + 8), pack32(tur + 12),
	      pack32(tur + 16), pack32(tur + 20), pack32(tur + 24), pack32(tur + 28),
	      cap0, cap1);
	for (;;) {
		IODelay(1000000);
	}
}

// Level 58 worker: v5 proved the whole query WRITE path is blocked (SET and CLEAR FLAG both
// GENERAL_FAILURE while READ FLAG succeeds) and v6 proved the device's own descriptors
// describe an enabled 256 GB LUN 0. Background from NEXT-AGENT-HANDOFF: ABL resets the UFS
// device before handing off, so waiting-for-fDeviceInit is the expected state — but the
// working stacks (ColorOS, EDK2 UfsPassThru) always send a NOP OUT/IN handshake BEFORE
// fDeviceInit, and we never have. v7 follows that order: NOP OUT (all-zero UCD), then
// TOGGLE FLAG fDeviceInit (opcode 0x08 — the third write opcode; on a 0 flag it sets it,
// same effect as SET but a different path), then poll READ FLAG for the self-clear, then
// TUR and capacity. Every step bounded; one panic prints everything.
void l58Worker(void *, wait_result_t)
{
	uint8_t *b = gBuildVa;
	uint8_t *ucd = b + 0x40;
	uintptr_t hc = gRingHc;
	const volatile uint8_t *rsp = reinterpret_cast<const volatile uint8_t *>(gL46Resp);
	uint8_t tur[48] = {};
	unsigned good = 0;

	// NOP OUT: TT 0x00, everything else zero (zeroWords does the whole UCD).
	uint8_t nopDbr = 0;
	uint8_t nopTt = 0xff;
	zeroWords(b, 32);
	zeroWords(ucd, 1040);
	writeUtrd(b, gDmaPhys, 0, false);
	cacheClean(b, 16384);
	writeReg(hc, SM_UFS_REG_IS, 0xffffffffU);
	writeReg(hc, SM_UFS_REG_UTRLDBR, 1);
	if (pollDoorbellClear(hc)) {
		nopDbr = 1;
		nopTt = rsp[0];
	}
	// TOGGLE FLAG fDeviceInit: flag is 0, so toggle = set.
	uint8_t tog6 = 0xff;
	zeroWords(b, 32);
	zeroWords(ucd, 1040);
	ucd[0] = 0x16;   // QUERY REQUEST
	ucd[5] = 0x81;   // standard write request
	ucd[12] = 0x08;  // opcode: TOGGLE FLAG
	ucd[13] = 0x01;  // IDN: fDeviceInit
	writeUtrd(b, gDmaPhys, 0, false);
	cacheClean(b, 16384);
	writeReg(hc, SM_UFS_REG_IS, 0xffffffffU);
	writeReg(hc, SM_UFS_REG_UTRLDBR, 1);
	if (pollDoorbellClear(hc)) {
		tog6 = rsp[6];
	}
	// Poll READ FLAG: 1 = initializing (watch it on the square strip), 0 = done or never set.
	uint32_t rfValue = 0;
	if (tog6 == 0x00) {
		for (unsigned attempt = 0; attempt < 100; attempt++) {
			zeroWords(b, 32);
			zeroWords(ucd, 1040);
			ucd[0] = 0x16;   // QUERY REQUEST
			ucd[5] = 0x01;   // standard read request
			ucd[12] = 0x05;  // opcode: READ FLAG
			ucd[13] = 0x01;  // IDN: fDeviceInit
			writeUtrd(b, gDmaPhys, 0, false);
			cacheClean(b, 16384);
			writeReg(hc, SM_UFS_REG_IS, 0xffffffffU);
			writeReg(hc, SM_UFS_REG_UTRLDBR, 1);
			if (!pollDoorbellClear(hc)) {
				break;
			}
			rfValue = pack32(rsp + 20);
			if (attempt < 100) {
				fbFill(gFb, 16 + attempt * 6, 1616, 6, 24,
				       (rfValue == 1) ? 0xff00ff00U : 0xff202020U);
			}
			if (rfValue == 0) {
				break;
			}
			IODelay(20000);
		}
	}

	zeroWords(b, 32);
	zeroWords(ucd, 1040);
	ucd[0] = 0x01;   // COMMAND UPIU
	ucd[1] = 0x00;   // no data phase
	ucd[2] = 0x00;   // LUN 0
	for (unsigned index = 0; index < 6; index++) {
		ucd[16 + index] = 0;   // TEST UNIT READY CDB
	}
	writeUtrd(b, gDmaPhys, 0, false);
	cacheClean(b, 16384);
	writeReg(hc, SM_UFS_REG_IS, 0xffffffffU);
	writeReg(hc, SM_UFS_REG_UTRLDBR, 1);
	if (pollDoorbellClear(hc)) {
		for (unsigned i = 0; i < 48; i++) {
			tur[i] = rsp[i];
		}
		fbFill(gFb, 608, 896, 24, 24, 0xff00ff00U);
		if (rsp[0] != 0 && rsp[7] == 0x00) {
			good = 1;
			fbFill(gFb, 640, 896, 24, 24, 0xff00ff00U);
		}
	}
	uint32_t cap0 = 0, cap1 = 0;
	unsigned capok = 0;
	uint32_t capOcs = 0xffffffffU;
	uint8_t capRsp[48] = {};
	{   // capacity gate removed: try READ CAPACITY regardless of TUR
		zeroWords(b, 32);
		zeroWords(ucd, 1040);
		ucd[0] = 0x01;   // COMMAND UPIU
		ucd[1] = 0x40;   // flags: read
		ucd[2] = 0x00;   // LUN 0
		putBe32(ucd + 12, 8);
		// zeroWords above already cleared the CDB; avoid merged unaligned stores here.
		ucd[16] = 0x25;  // READ CAPACITY(10) opcode at CDB byte 0
		writeUtrd(b, gDmaPhys, 8, true);
		cacheClean(b, 16384);
		writeReg(hc, SM_UFS_REG_IS, 0xffffffffU);
		writeReg(hc, SM_UFS_REG_UTRLDBR, 1);
		if (pollDoorbellClear(hc)) {
			capok = 1;
			fbFill(gFb, 672, 896, 24, 24, 0xff00ff00U);
			const volatile uint8_t *cap = reinterpret_cast<const volatile uint8_t *>(gCapVa);
			const volatile uint8_t *utrd = reinterpret_cast<const volatile uint8_t *>(gL46Utrd);
			cap0 = pack32(cap + 0);
			cap1 = pack32(cap + 4);
			capOcs = utrd[8] | ((uint32_t)utrd[9] << 8) |
			         ((uint32_t)utrd[10] << 16) | ((uint32_t)utrd[11] << 24);
			for (unsigned i = 0; i < sizeof(capRsp); i++) {
				capRsp[i] = rsp[i];
			}
		}
	}
	panic("SMUFS capdiag v7 probe58\n"
	      "settries %u set6 %02x\n"
	      "nop tt %02x dbr %x\n"
	      "tog6 %02x rf %08x\n"
	      "turgood %u capdone %u capocs %08x\n"
	      "cap %08x %08x\n"
	      "cpr %08x %08x\n"
	      "hcs %08x",
	      gSettries, gSetresp[6],
	      nopTt, nopDbr,
	      tog6, rfValue,
	      good, capok, capOcs,
	      cap0, cap1,
	      pack32(capRsp + 0), pack32(capRsp + 4),
	      readReg(hc, SM_UFS_REG_HCS));
	for (;;) {
		IODelay(1000000);
	}
}

void ringAndReport(uintptr_t hc, const SmUfsDma &dma)
{
	gRingHc = hc;
	gRingDone = 0;
	thread_t thread = nullptr;
	bool started = kernel_thread_start(ringThread, nullptr, &thread) == KERN_SUCCESS;
	if (started) {
		thread_deallocate(thread);
	}
	for (unsigned loop = 0; loop < 20; loop++) {
		IODelay(1000);
		if (gRingDone) {
			break;
		}
	}
	OSSharedPtr<IOMemoryMap> smmuMap;
	uintptr_t s = mapPhys(0x15000000ULL, 0xd0000, smmuMap);
	uint32_t gfsr = s ? readReg(s, 0x48) : 0xdeadbeefU;
	uint32_t syn0 = s ? readReg(s, 0x50) : 0;
	uint32_t syn1 = s ? readReg(s, 0x54) : 0;
	uint32_t scr0 = s ? readReg(s, 0x0) : 0;
	uint32_t s2cr0 = s ? readReg(s, 0xc00) : 0;
	kprintf("SMUFS ring8 gfsr %08x syn %08x %08x scr0 %08x s2cr0 %08x\n", gfsr, syn0, syn1,
	        scr0, s2cr0);
	OSSharedPtr<IOMemoryMap> utrdMap;
	uintptr_t u = mapPhys(dma.phys, 32, utrdMap);
	uint32_t w0 = u ? readReg(u, 0) : 0xdeadbeefU;
	uint32_t w2 = u ? readReg(u, 8) : 0xdeadbeefU;
	kprintf("SMUFS ring8 utrd %08x %08x\n", w0, w2);
	kprintf("SMUFS ring8 start %x done %x dbr %08x is %08x hcs %08x\n", started, gRingDone,
	        readReg(hc, SM_UFS_REG_UTRLDBR), readReg(hc, SM_UFS_REG_IS),
	        readReg(hc, SM_UFS_REG_HCS));
	panic("SMUFS ring8 done=%x", gRingDone);
}

// One polled command through request slot 0. The UTRD, command UPIU, response UPIU and PRDT
// live at fixed offsets in the DMA buffer, as in the sheng transport.
SmUfsCommandResult submitCommand(uintptr_t hc, const SmUfsDma &dma, uint8_t upiuType, uint8_t lun,
                                 const uint8_t *cdb, unsigned cdbLength, bool toHost, uint32_t length)
{
	SmUfsCommandResult result = {0xf, 0xff, 0xff, 0};
	uint8_t *utrd = dma.virt;
	uint8_t *ucd = dma.virt + kUcdOffset;
	zeroWords(utrd, 32);
	zeroWords(ucd, 1040);
	if (length) {
		zeroWords(dma.virt + kDataOffset, length);
	}

	ucd[0] = upiuType;
	ucd[1] = toHost ? 0x40 : 0x00;
	ucd[2] = lun;
	ucd[3] = 0;
	putBe32(ucd + 12, length);
	for (unsigned index = 0; index < cdbLength; index++) {
		ucd[16 + index] = cdb[index];
	}

	uint64_t ucdPhys = dma.phys + kUcdOffset;
	*reinterpret_cast<uint32_t *>(utrd + 0) = 0x10000000U | ((length ? (toHost ? 2U : 1U) : 0U) << 25);
	*reinterpret_cast<uint32_t *>(utrd + 8) = 0x0fU;
	*reinterpret_cast<uint32_t *>(utrd + 16) = (uint32_t)ucdPhys;
	*reinterpret_cast<uint32_t *>(utrd + 20) = (uint32_t)(ucdPhys >> 32);
	*reinterpret_cast<uint32_t *>(utrd + 24) = (512U >> 2) | ((kRspOffset >> 2) << 16);
	*reinterpret_cast<uint32_t *>(utrd + 28) = (length ? 1U : 0U) | ((kPrdtOffset >> 2) << 16);
	if (length) {
		uint64_t dataPhys = dma.phys + kDataOffset;
		uint8_t *prd = ucd + kPrdtOffset;
		*reinterpret_cast<uint32_t *>(prd + 0) = (uint32_t)dataPhys;
		*reinterpret_cast<uint32_t *>(prd + 4) = (uint32_t)(dataPhys >> 32);
		*reinterpret_cast<uint32_t *>(prd + 12) = length - 1;
	}

	cacheClean(dma.virt, kDataOffset + length);

	// Read the descriptor back through a second, uncached mapping before the doorbell. A controller
	// that fetches a zero descriptor takes its UCD address from word four, i.e. address zero, and
	// that fetch can wedge the bus. This build reports the check and does not ring.
	{
		OSSharedPtr<IOMemoryMap> checkMap;
		uintptr_t checkBase = mapPhys(dma.phys, 64, checkMap);
		uint32_t words[8] = {0};
		if (checkBase != 0) {
			for (unsigned index = 0; index < 8; index++) {
				words[index] = readReg(checkBase, index * 4);
			}
		}
		for (unsigned index = 0; index < 8; index++) {
			gSmUfs.utrdCheck[index] = words[index];
		}
		if (checkBase == 0 || words[4] != (uint32_t)ucdPhys || words[5] != (uint32_t)(ucdPhys >> 32)) {
			result.ocs = 0xfe;
			return result;
		}
	}
	// Levels 4 and 6 stop before the doorbell. Level 6 also acknowledges the UIC error and polls
	// long enough to tell a stale latch from a source that keeps re-arming it; level 4 only takes
	// the codes. The earlier builds printed uicErr before it was ever read, which made IS bit 2
	// (UIC_ERROR) look like a field of zeros.
	if (gSmUfs.level == 4 || gSmUfs.level == 6) {
		acknowledgeUicErrors(hc, gSmUfs.level == 6);
		uintptr_t gcc = gSmUfs.gccBase;
		SmUfsRcg axi, unip, ice, aux;
		readRcg(gcc, SM_GCC_UFS_AXI_RCG, axi);
		readRcg(gcc, SM_GCC_UFS_UNIPRO_RCG, unip);
		readRcg(gcc, SM_GCC_UFS_ICE_RCG, ice);
		readRcg(gcc, SM_GCC_UFS_PHY_AUX_RCG, aux);
	// ufs_qcom_link_startup_post_change's clock-request bit, for the record.
	gSmUfs.ah8Cfg = readReg(hc, 0xfc);
	panic("SMUFS uic is %08x>%08x ret %x\n"
		      "uicerr pa %08x dl %08x nl %08x tl %08x dme %08x\n"
		      "clkreq %08x\n"
		      "hcs %08x hce %08x dbr %08x rsr %08x\n"
		      "uiccmd %08x arg1 %08x arg2 %08x arg3 %08x\n"
		      "utrd list %08x w %08x %08x %08x %08x\n"
		      "ice ver %08x ctrl %08x byp %08x bist %08x\n"
		      "tim %08x %08x %08x cfg2 %08x cfg1 %08x\n"
		      "gdsc %08x cap %08x ver %08x pcs %08x\n"
		      "hwver %08x cfg1 %08x>%08x iceadv %08x>%08x polls %x\n"
		      "rcg axi  cmd %08x cfg %08x m %08x n %08x d %08x fix %x upd %x\n"
		      "rcg unip cmd %08x cfg %08x m %08x n %08x d %08x fix %x upd %x\n"
		      "rcg ice  cmd %08x cfg %08x m %08x n %08x d %08x fix %x upd %x\n"
		      "rcg aux  cmd %08x cfg %08x m %08x n %08x d %08x fix %x upd %x\n"
		      "smmu first smr %08x fsr %08x gfsr %08x scr0 %08x\n"
		      "cbcr ahb %08x tx0 %08x rx0 %08x rx1 %08x aux %08x\n",
		      gSmUfs.uicIsBefore, gSmUfs.uicIsAfter, gSmUfs.uicReturns,
		      gSmUfs.uicErr[0], gSmUfs.uicErr[1], gSmUfs.uicErr[2], gSmUfs.uicErr[3],
		      gSmUfs.uicErr[4],
		      gSmUfs.ah8Cfg,
		      readReg(hc, SM_UFS_REG_HCS), readReg(hc, SM_UFS_REG_HCE),
		      readReg(hc, SM_UFS_REG_UTRLDBR), readReg(hc, SM_UFS_REG_UTRLRSR),
		      readReg(hc, SM_UFS_REG_UICCMD), readReg(hc, SM_UFS_REG_UICARG1),
		      readReg(hc, SM_UFS_REG_UICARG2), readReg(hc, SM_UFS_REG_UICARG3),
		      gSmUfs.dmaPhys, gSmUfs.utrdCheck[0], gSmUfs.utrdCheck[2],
		      gSmUfs.utrdCheck[4], gSmUfs.utrdCheck[6],
		      gSmUfs.iceVersion, gSmUfs.iceCtrl, gSmUfs.iceBypass, gSmUfs.iceBist,
		      gSmUfs.timC0, gSmUfs.timC4, gSmUfs.timD8, readReg(hc, SM_UFS_REG_CFG2),
		      readReg(hc, SM_UFS_REG_CFG1),
		      readReg(gcc, (uint32_t)(SM_UFS_PHY_GDSC - SM_GCC_BASE)),
		      readReg(hc, SM_UFS_REG_CAP), readReg(hc, SM_UFS_REG_VERSION),
		      readReg(gSmUfs.phyBase, SM_PHY_PCS_READY),
		      gSmUfs.hwVersion, gSmUfs.cfg1Before, gSmUfs.cfg1After,
		      gSmUfs.iceAdvBefore, gSmUfs.iceAdvAfter, gSmUfs.iceBistPolls,
		      axi.cmd, axi.cfg, axi.m, axi.n, axi.d, gSmUfs.rcgFix[0], gSmUfs.rcgUpdate[0],
		      unip.cmd, unip.cfg, unip.m, unip.n, unip.d, gSmUfs.rcgFix[1], gSmUfs.rcgUpdate[1],
		      ice.cmd, ice.cfg, ice.m, ice.n, ice.d, gSmUfs.rcgFix[2], gSmUfs.rcgUpdate[2],
		      aux.cmd, aux.cfg, aux.m, aux.n, aux.d, gSmUfs.rcgFix[3], gSmUfs.rcgUpdate[3],
		      gSmUfs.bypassS2crBefore, gSmUfs.bypassFsr, gSmUfs.bypassGfsr, gSmUfs.bypassScr0,
		      readReg(gcc, SM_GCC_UFS_AHB_CBCR), readReg(gcc, SM_GCC_UFS_TX_SYMBOL0_CBCR),
		      readReg(gcc, SM_GCC_UFS_RX_SYMBOL0_CBCR), readReg(gcc, SM_GCC_UFS_RX_SYMBOL1_CBCR),
		      readReg(gcc, SM_GCC_UFS_PHY_AUX_CBCR));
	}
	// Levels 3, 5 and 7 acknowledge, then ring. Level 7 polls and prints the result before the
	// doorbell, so a hang still leaves the error codes on the screen.
	acknowledgeUicErrors(hc, gSmUfs.level >= 7);
	if (gSmUfs.level >= 7) {
		kprintf("SMUFS uic ack is %08x>%08x ret %x err %08x %08x %08x %08x %08x\n",
		        gSmUfs.uicIsBefore, gSmUfs.uicIsAfter, gSmUfs.uicReturns,
		        gSmUfs.uicErr[0], gSmUfs.uicErr[1], gSmUfs.uicErr[2], gSmUfs.uicErr[3],
		        gSmUfs.uicErr[4]);
	}
	fbMark2(1);
	// A zero doorbell is a no-op in the register. If even this write stalls, the UTP slave is
	// holding the register access itself; if it returns and the real doorbell hangs, the stall is
	// in the transfer fetch.
	writeReg(hc, SM_UFS_REG_UTRLDBR, 0);
	fbMark2(2);
	if (gSmUfs.level == 8) {
		ringAndReport(hc, dma);
	}
	writeReg(hc, SM_UFS_REG_UTRLDBR, 1);
	if (gSmUfs.level == 5) {
		panic("SMUFS ring returned dbr %08x is %08x hcs %08x",
		      readReg(hc, SM_UFS_REG_UTRLDBR), readReg(hc, SM_UFS_REG_IS),
		      readReg(hc, SM_UFS_REG_HCS));
	}
	fbMark2(3);
	for (uint32_t loops = 0; loops < 100000; loops++) {
		if ((readReg(hc, SM_UFS_REG_UTRLDBR) & 1) == 0) {
			break;
		}
		IODelay(1);
	}
	fbMark2(4);
	cacheCleanInvalidate(dma.virt, kDataOffset + length);
	result.ocs = *reinterpret_cast<volatile uint32_t *>(utrd + 8) & 0xff;
	const uint8_t *rsp = ucd + kRspOffset;
	result.response = rsp[6];
	result.scsi = rsp[7];
	return result;
}

// Read-only apps-SMMU inventory. The UFS doc requires the stream and context ownership before the
// request list is touched; if 0x60 (the Qualcomm UFS stream) has a valid match, UTRLBA is an IOVA
// and physical DMA is wrong.
void snapshotSmmu(void)
{
	OSSharedPtr<IOMemoryMap> map;
	uintptr_t smmu = mapPhys(0x15000000ULL, 0xd0000, map);
	gSmUfs.smmuIdx60 = 0xffffffffU;
	gSmUfs.smmuIdx1 = 0xffffffffU;
	if (smmu == 0) {
		return;
	}
	gSmUfs.smmuScr0 = readReg(smmu, 0x000);
	gSmUfs.smmuAcr = readReg(smmu, 0x010);
	gSmUfs.smmuIdr0 = readReg(smmu, 0x020);
	gSmUfs.smmuIdr1 = readReg(smmu, 0x024);
	uint32_t groups = gSmUfs.smmuIdr0 & 0x7f;
	if (groups == 0) {
		return;
	}
	for (uint32_t index = 0; index < groups; index++) {
		uint32_t smr = readReg(smmu, 0x800 + 4 * index);
		if ((smr & 0x80000000U) == 0) {
			continue;
		}
		gSmUfs.smmuValid++;
		// SMR.SID[15:0] covers the stream; the mask widens the match.
		uint32_t mask = (smr >> 16) & 0x7fff;
		uint32_t s2cr = readReg(smmu, 0xc00 + 4 * index);
		if (gSmUfs.smmuIdx1 == 0xffffffffU) {
			// The one stream the firmware configured: its ID, type and context tell us whether UFS
			// is translated, bypassed or pointed at a fault.
			uint32_t bank = s2cr & 0xff;
			uint32_t pages = 1U << (((gSmUfs.smmuIdr1 >> 28) & 7) + 1);
			uintptr_t cb = smmu + (pages + bank) * 4096;
			gSmUfs.smmuIdx1 = index;
			gSmUfs.smmuSid1 = smr & 0x7fff;
			gSmUfs.smmuMask1 = mask;
			gSmUfs.smmuS2cr1 = s2cr;
			if (bank < (gSmUfs.smmuIdr1 & 0xff)) {
				gSmUfs.smmuCbar1 = readReg(smmu, 0x1000 + 4 * bank);
				gSmUfs.smmuSctlr1 = readReg(cb, 0x0);
				gSmUfs.smmuTtbr01 = readReg(cb, 0x20);
			}
		}
		if (((0x60 & ~mask) == ((smr & 0x7fff) & ~mask)) && gSmUfs.smmuIdx60 == 0xffffffffU) {
			gSmUfs.smmuIdx60 = index;
			gSmUfs.smmuS60 = smr;
			gSmUfs.smmuS2c60 = s2cr;
		}
	}
}

// Read-only apps-SMMU inspection. On kona the UFS node has no iommus and no SID of its own: the
// firmware leaves it as an unmatched stream, so its DMA is physical while sCR0.USFCFG is clear.
// The only valid stream-match entry on this SMMU belongs to the display (0x820 mask 0x402,
// kona-sde.dtsi), so writing S2CR here hijacks the panel's stream: the old write turned it into a
// translation through CB0, and a TYPE=BYPASS write is rejected by this firmware and reads back as
// FAULT, which blanks the display the moment the DPU's next fetch faults. Look, do not touch.
int inspectSmmu(void)
{
	OSSharedPtr<IOMemoryMap> map;
	uintptr_t s = mapPhys(0x15000000ULL, 0xd0000, map);
	if (s == 0) {
		return 1;
	}
	uint32_t idr1 = readReg(s, 0x24);
	uint32_t groups = readReg(s, 0x20) & 0x7f;
	uint32_t pages = 1U << (((idr1 >> 28) & 7) + 1);
	gSmUfs.smmuPages = pages;
	gSmUfs.bypassSid = 0xffffffffU;
	gSmUfs.bypassS2crBefore = 0;
	gSmUfs.bypassS2crAfter = 0;
	gSmUfs.bypassFsr = 0;
	gSmUfs.bypassGfsr = readReg(s, SM_SMMU_NS_SGFSR);
	for (uint32_t index = 0; index < groups; index++) {
		uint32_t smr = readReg(s, 0x800 + 4 * index);
		if ((smr & 0x80000000U) == 0) {
			continue;
		}
		uint32_t s2cr = readReg(s, 0xc00 + 4 * index);
		uint32_t bank = s2cr & 0xff;
		gSmUfs.bypassSid = smr & 0x7fff;
		gSmUfs.bypassS2crBefore = s2cr;
		gSmUfs.bypassS2crAfter = s2cr;
		if (bank < (idr1 & 0xff)) {
			uintptr_t cb = s + (pages + bank) * 4096;
			gSmUfs.bypassFsr = readReg(cb, SM_SMMU_CB_FSR);
		}
		// Read-only: the firmware's unmatched-stream policy is what made its own UFS work, and
		// porting.toml records that no apps-SMMU takeover is needed on this board.
		uint32_t scr0 = readReg(s, 0x0);
		gSmUfs.bypassScr0 = scr0;
		kprintf("SMUFS smmu scr0 %08x sid %x s2cr %x\n", scr0, gSmUfs.bypassSid, s2cr);
		return 0;
	}
	return 4;
}

// Called from the watchdog on another CPU after the probe stopped making progress: everything the
// probe would have reported at the end, read live. The UFS block is read last because the stalled
// doorbell may hold its slave; the SMMU, NoC and ICE windows are separate and safe to read.
void dumpHangState(void)
{
	// A stalled UFS transaction can take the whole bus with it, so nothing here may be batched:
	// every group of reads is printed before the next block is touched.
	kprintf("SMUFS hang begin prog=%x dma %08x\n", gSmUfs.progress, gSmUfs.dmaPhys);
	OSSharedPtr<IOMemoryMap> smmuMap;
	uintptr_t s = mapPhys(0x15000000ULL, 0xd0000, smmuMap);
	if (s != 0) {
		uint32_t gfsr = readReg(s, 0x48);
		kprintf("SMUFS hang gfsr %08x\n", gfsr);
		uint32_t syn0 = readReg(s, 0x50);
		uint32_t syn1 = readReg(s, 0x54);
		kprintf("SMUFS hang gfsynr %08x %08x\n", syn0, syn1);
		kprintf("SMUFS hang scr0 %08x s2cr0 %08x\n", readReg(s, 0x0), readReg(s, 0xc00));
		uint32_t pages = gSmUfs.smmuPages ? gSmUfs.smmuPages : 16;
		uintptr_t cb = s + pages * 4096;
		kprintf("SMUFS hang cb0 fsr %08x fsynr %08x\n", readReg(cb, SM_SMMU_CB_FSR),
		        readReg(cb, SM_SMMU_CB_FSYNR0));
	}
	OSSharedPtr<IOMemoryMap> nocMap;
	uintptr_t n = mapPhys(0x16e0000ULL, 0x20000, nocMap);
	if (n != 0) {
		kprintf("SMUFS hang noc mas7b mode %08x prio %08x\n",
		        readReg(n, 0x500c), readReg(n, 0x5008));
	}
	OSSharedPtr<IOMemoryMap> iceMap;
	uintptr_t ice = mapPhys(0x1d90000ULL, 0x8000, iceMap);
	if (ice != 0) {
		kprintf("SMUFS hang ice ctrl %08x byp %08x bist %08x\n",
		        readReg(ice, 0x0), readReg(ice, 0x74), readReg(ice, 0x70));
	}
	if (gSmUfs.dmaPhys != 0) {
		OSSharedPtr<IOMemoryMap> utrdMap;
		uintptr_t u = mapPhys(gSmUfs.dmaPhys, 32, utrdMap);
		if (u != 0) {
			uint32_t w0 = readReg(u, 0);
			uint32_t w2 = readReg(u, 8);
			uint32_t w4 = readReg(u, 16);
			uint32_t w6 = readReg(u, 24);
			kprintf("SMUFS hang utrd %08x %08x %08x %08x\n", w0, w2, w4, w6);
		}
	}
	kprintf("SMUFS hang ufs is %08x hcs %08x hce %08x dbr %08x rsr %08x\n",
	        readReg(gSmUfs.hcBase, SM_UFS_REG_IS), readReg(gSmUfs.hcBase, SM_UFS_REG_HCS),
	        readReg(gSmUfs.hcBase, SM_UFS_REG_HCE), readReg(gSmUfs.hcBase, SM_UFS_REG_UTRLDBR),
	        readReg(gSmUfs.hcBase, SM_UFS_REG_UTRLRSR));
}

int adoptAndIdentify(uintptr_t hc, SmUfsDma &dma)
{
	// Vendor ufshcd order: stop the controller, program both request lists, enable it, run one
	// DME_LINK_STARTUP (the only link-start UIC command this platform sends), require
	// HCS.DEVICE_PRESENT, then start both queues and require the four ready bits.
	gSmUfs.progress = 1;
	fbMark(1);
	writeReg(hc, SM_UFS_REG_UTRLRSR, 0);
	writeReg(hc, SM_UFS_REG_UTMRLRSR, 0);
	writeReg(hc, 0x24, 0x470);   // UIC completion, power mode, hibernate enter/exit
	writeReg(hc, SM_UFS_REG_HCE, 0);
	bool off = false;
	for (unsigned loop = 0; loop < 10000; loop++) {
		if ((readReg(hc, SM_UFS_REG_HCE) & 1) == 0) { off = true; break; }
		IODelay(10);
	}
	fbMark(2);
	if (!off) {
		return 1;
	}
	// ufshcd_vops_hce_enable_notify(PRE_CHANGE) runs ufs_qcom_power_up_sequence() with the
	// controller stopped, and the last act of that sequence is ufs_qcom_select_unipro_mode():
	// for a v2+ controller CFG1.QUNIPRO_SEL must be set before the controller is enabled. The
	// firmware's failed UFS DXE leaves it clear, and a controller in the wrong UniPro mode can
	// still answer UIC commands while its request-list path never accepts a doorbell.
	gSmUfs.hwVersion = readReg(hc, SM_UFS_REG_HW_VERSION);
	gSmUfs.cfg1Before = readReg(hc, SM_UFS_REG_CFG1);
	gSmUfs.cfg1After = gSmUfs.cfg1Before;
	if (gSmUfs.level >= 6) {
		writeReg(hc, SM_UFS_REG_CFG1, gSmUfs.cfg1Before | SM_UFS_CFG1_QUNIPRO_SEL);
		gSmUfs.cfg1After = readReg(hc, SM_UFS_REG_CFG1);
		kprintf("SMUFS cfg1 %08x>%08x hwver %08x\n", gSmUfs.cfg1Before, gSmUfs.cfg1After,
		        gSmUfs.hwVersion);
	}
	writeReg(hc, SM_UFS_REG_IS, 0xffffffffU);
	if (gSmUfs.level == 32) {
		// The vendor also points the task-management list at the same page; the bootloader's
		// value now addresses recycled heap, and the engine may touch it on a doorbell.
		writeReg(hc, SM_UFS_REG_UTMRLBA, (uint32_t)dma.phys);
		writeReg(hc, SM_UFS_REG_UTMRLBAU, (uint32_t)(dma.phys >> 32));
	}
	writeReg(hc, SM_UFS_REG_UTRLBA, (uint32_t)dma.phys);
	writeReg(hc, SM_UFS_REG_UTRLBAU, (uint32_t)(dma.phys >> 32));
	writeReg(hc, SM_UFS_REG_UTMRLBA, (uint32_t)(dma.phys + kUtmOffset));
	writeReg(hc, SM_UFS_REG_UTMRLBAU, (uint32_t)((dma.phys + kUtmOffset) >> 32));
	// The vendor sets CRYPTO_GENERAL_ENABLE with the controller enable only after the ICE is
	// enabled: advanced control, BIST completion and the non-secure IRQ unmask. With the ICE bit
	// clear the data path stays gated internally even for bypass requests.
	{
		OSSharedPtr<IOMemoryMap> iceMap;
		uintptr_t ice = mapPhys(0x1d90000ULL, 0x8000, iceMap);
		if (ice != 0) {
			// qcom_ice_low_power_mode_enable() + qcom_ice_optimization_enable(): both are read-
			// modify-write in ice.c; writing the constant would clear whatever else the register
			// holds. 0xd807100 already carries the 0x7000 low-power bits.
			gSmUfs.iceAdvBefore = readReg(ice, 0x1000);
			writeReg(ice, 0x1000, gSmUfs.iceAdvBefore | 0x0d807100U);
			gSmUfs.iceAdvAfter = readReg(ice, 0x1000);
			for (unsigned loop = 0; loop < 10000; loop++) {
				if ((readReg(ice, 0x070) & 0xf0000000U) == 0) break;
				IODelay(10);
			}
			writeReg(ice, 0x048, 0);
			gSmUfs.iceBist = readReg(ice, 0x070);
		}
	}
	writeReg(hc, SM_UFS_REG_HCE, 3);   // CONTROLLER_ENABLE | CRYPTO_GENERAL_ENABLE
	bool on = false;
	for (unsigned loop = 0; loop < 10000; loop++) {
		if (readReg(hc, SM_UFS_REG_HCE) & 1) { on = true; break; }
		IODelay(10);
	}
	fbMark(3);
	if (!on) {
		return 2;
	}
	int link = 3;
	for (unsigned attempt = 0; attempt < 3; attempt++) {
		writeReg(hc, SM_UFS_REG_IS, SM_UIC_COMMAND_COMPL);
		writeReg(hc, SM_UFS_REG_UICARG1, 0);
		writeReg(hc, SM_UFS_REG_UICARG2, 0);
		writeReg(hc, SM_UFS_REG_UICARG3, 0);
		writeReg(hc, SM_UFS_REG_UICCMD, SM_UIC_CMD_DME_LINK_STARTUP);
		bool done = false;
		for (unsigned loop = 0; loop < 10000; loop++) {
			if (readReg(hc, SM_UFS_REG_IS) & SM_UIC_COMMAND_COMPL) { done = true; break; }
			IODelay(10);
		}
		// The UIC result is UICCMDARG2 low byte; UICCMDARG3 carries DME read data.
		gSmUfs.uicResult = done ? (readReg(hc, SM_UFS_REG_UICARG2) & 0xff) : 0x55;
		writeReg(hc, SM_UFS_REG_IS, SM_UIC_COMMAND_COMPL);
		gSmUfs.uicHcs = readReg(hc, SM_UFS_REG_HCS);
		if (done && gSmUfs.uicResult == 0 && (gSmUfs.uicHcs & 1)) {
			link = 0;
			break;
		}
	}
	fbMark(4);
	if (link != 0) {
		return 3;
	}

	// ufs_qcom_link_startup_post_change's last step, which this driver never did: enable the
	// controller's clock-request lines. The firmware leaves CFG2's UTP clock-gating cells on
	// (cfg2 low byte 0xff), so an idle UTP RAM is gated; without these clk_req bits the doorbell
	// has no way to wake it and the write stalls even though every status register reads ready.
	gSmUfs.ah8Cfg = readReg(hc, 0xfc);
	writeReg(hc, 0xfc, gSmUfs.ah8Cfg | 0x3c);   // SYS | ICE | UNIPRO | AUX clk_req
	gSmUfs.ah8Cfg = readReg(hc, 0xfc);

	// Vendor link-startup pre/post hooks: keep the controller timers, enable the sub-block clock
	// gating, clear the transmit LCC and disarm auto-hibernate; then read the ICE state and force
	// global bypass when it is not bypassed, because a keyed or BIST-stuck ICE can hold transfers.
	// Vendor cfg_timers: 0xC0 is the core clock in MHz and 0xD8 the link-startup timeout in
	// core clock cycles. Keep a valid value instead of whatever the bootloader left.
	gSmUfs.timC0 = readReg(hc, 0xc0);
	gSmUfs.timC4 = readReg(hc, 0xc4);
	gSmUfs.timD8 = readReg(hc, 0xd8);
	if (gSmUfs.timC0 == 0 || gSmUfs.timC0 > 1000) {
		gSmUfs.timC0 = 300;   // freq-table maximum for core_clk
		writeReg(hc, 0xc0, gSmUfs.timC0);
	}
	if (gSmUfs.timD8 == 0) {
		gSmUfs.timD8 = gSmUfs.timC0 * 10000;
		writeReg(hc, 0xd8, gSmUfs.timD8);
	}
	// Re-read the clock roots after link startup. A root that was running when the controller was
	// taken over can still fall back to stopped if nothing downstream holds it; this is the last
	// clock state before the first doorbell.
	for (unsigned index = 0; index < 4; index++) {
		readRcg(gSmUfs.gccBase, kRcgOffsets[index], gSmUfs.rcgLate[index]);
		kprintf("SMUFS rcg late %s cmd %08x cfg %08x\n", kRcgNames[index],
		        gSmUfs.rcgLate[index].cmd, gSmUfs.rcgLate[index].cfg);
	}
	gSmUfs.cfg2 = readReg(hc, 0xe0);
	writeReg(hc, 0xe0, gSmUfs.cfg2 | 0xff);
	gSmUfs.ah8Timer = readReg(hc, 0x18);
	writeReg(hc, 0x18, 0);
	{
		// DME_SET(PA_LOCAL_TX_LCC_ENABLE = 0x155e), value 0.
		writeReg(hc, SM_UFS_REG_IS, SM_UIC_COMMAND_COMPL);
		writeReg(hc, SM_UFS_REG_UICARG1, 0x155eU << 16);
		writeReg(hc, SM_UFS_REG_UICARG2, 0);
		writeReg(hc, SM_UFS_REG_UICARG3, 0);
		writeReg(hc, SM_UFS_REG_UICCMD, 0x02);
		for (unsigned loop = 0; loop < 10000; loop++) {
			if (readReg(hc, SM_UFS_REG_IS) & SM_UIC_COMMAND_COMPL) break;
			IODelay(10);
		}
		writeReg(hc, SM_UFS_REG_IS, SM_UIC_COMMAND_COMPL);
	}
	{
		OSSharedPtr<IOMemoryMap> iceMap;
		uintptr_t ice = mapPhys(0x1d90000ULL, 0x8000, iceMap);
		if (ice != 0) {
			gSmUfs.iceVersion = readReg(ice, 0x008);
			gSmUfs.iceCtrl = readReg(ice, 0x000);
			gSmUfs.iceBypass = readReg(ice, 0x074);
			// qcom_ice_wait_bist_status(): the vendor will not use the engine until the BIST
			// bits clear. Poll once more after the controller enable, which is what starts it.
			gSmUfs.iceBistPolls = 0;
			for (unsigned loop = 0; loop < 2000; loop++) {
				if ((readReg(ice, 0x070) & 0xf0000000U) == 0) break;
				gSmUfs.iceBistPolls = loop;
				IODelay(10);
			}
			gSmUfs.iceBistFinal = readReg(ice, 0x070);
			gSmUfs.iceBist = gSmUfs.iceBistFinal;
			if ((gSmUfs.iceBypass & 0x80000000U) == 0) {
				writeReg(ice, 0x000, gSmUfs.iceCtrl | 1);
				gSmUfs.iceCtrl = readReg(ice, 0x000);
				gSmUfs.iceBypass = readReg(ice, 0x074);
			}
		}
	}
	writeReg(hc, SM_UFS_REG_UTRLRSR, 1);
	writeReg(hc, SM_UFS_REG_UTMRLRSR, 1);
	bool ready = false;
	for (unsigned loop = 0; loop < 10000; loop++) {
		if ((readReg(hc, SM_UFS_REG_HCS) & 0x0f) == 0x0f) { ready = true; break; }
		IODelay(10);
	}
	gSmUfs.rsrAfter = readReg(hc, SM_UFS_REG_UTRLRSR);
	gSmUfs.hcsAfter = readReg(hc, SM_UFS_REG_HCS);
	fbMark(5);
	if (!ready) {
		return 4;
	}

	// NOP OUT: the transport answers before any SCSI work.
	uint8_t nop[1] = {0};
	SmUfsCommandResult nopResult = submitCommand(hc, dma, 0x00, 0, nop, 0, false, 0);
	gSmUfs.nopOcs = nopResult.ocs;
	gSmUfs.nopResp = nopResult.response;
	gSmUfs.nopScsi = nopResult.scsi;
	gSmUfs.progress = 6;
	fbMark(6);
	kprintf("SMUFS nop ocs=%x rsp=%x scsi=%x\n", nopResult.ocs, nopResult.response, nopResult.scsi);

	// START STOP UNIT with power condition ACTIVE to the UFS Device well-known LU 0xD0: the
	// firmware leaves the device in UFS-PowerDown and normal LUs answer NOT READY until this wake.
	uint8_t wakeCdb[6] = {0x1b, 0, 0, 0, 0x10, 0};
	SmUfsCommandResult wake = submitCommand(hc, dma, 0x01, 0xd0, wakeCdb, 6, false, 0);
	gSmUfs.wakeOcs = wake.ocs;
	gSmUfs.wakeResp = wake.response;
	gSmUfs.wakeScsi = wake.scsi;
	gSmUfs.progress = 6;
	kprintf("SMUFS wake ocs=%x rsp=%x scsi=%x\n", wake.ocs, wake.response, wake.scsi);

	// READ CAPACITY(10): last LBA and block length.
	uint8_t capacityCdb[10] = {0x25};
	SmUfsCommandResult capacity = submitCommand(hc, dma, 0x01, 0, capacityCdb, 10, true, 8);
	gSmUfs.capOcs = capacity.ocs;
	gSmUfs.capResp = capacity.response;
	gSmUfs.capScsi = capacity.scsi;
	if (capacity.ocs == 0) {
		gSmUfs.capLastLba = loadBe32(dma.virt + kDataOffset);
		gSmUfs.capBlockLen = loadBe32(dma.virt + kDataOffset + 4);
	}
	gSmUfs.progress = 6;
	fbMark(7);
	kprintf("SMUFS cap ocs=%x rsp=%x scsi=%x lba=%08x blk=%08x\n", capacity.ocs, capacity.response, capacity.scsi, gSmUfs.capLastLba, gSmUfs.capBlockLen);

	// READ(10) LBA 0: the GPT header is the second 512-byte block.
	uint8_t readCdb[10] = {0x28, 0, 0, 0, 0, 0, 0, 0, 1, 0};
	SmUfsCommandResult read = submitCommand(hc, dma, 0x01, 0, readCdb, 10, true, 4096);
	gSmUfs.readOcs = read.ocs;
	gSmUfs.readResp = read.response;
	gSmUfs.readScsi = read.scsi;
	uint8_t *data = dma.virt + kDataOffset;
	gSmUfs.readW0 = loadBe32(data + 0);
	gSmUfs.readW1 = loadBe32(data + 4);
	gSmUfs.readGpt0 = loadBe32(data + 512);
	gSmUfs.readGpt1 = loadBe32(data + 516);
	gSmUfs.progress = 7;
	fbMark(8);
	kprintf("SMUFS read ocs=%x rsp=%x scsi=%x gpt=%08x %08x\n", read.ocs, read.response, read.scsi, gSmUfs.readGpt0, gSmUfs.readGpt1);
	gSmUfs.dbrAfter = readReg(hc, SM_UFS_REG_UTRLDBR);
	return 0;
}

}  // namespace

// Defined with the level-17 ring helper; the watchdog (member function) calls it.
void ringReportPanic(void);

#include "Probe70Root.h"

class Sm8250UfsBlock;
class Probe65Context;

class NativeUfs : public IOService {
	OSDeclareDefaultStructors(NativeUfs)

public:
	IOService *probe(IOService *provider, SInt32 *score) override;
	bool start(IOService *provider) override;
	void stop(IOService *provider) override;
	Probe65Context *blockContext = nullptr;
	IOLock *blockStartupGate = nullptr;
	uint32_t blockCancelled = 0;

private:
	static void probeThread(void *argument, wait_result_t result);
	static void watchdogThread(void *argument, wait_result_t result);
	void runProbe(uint32_t level);
	int  coldStart(SmUfsMaps &map);
	int  adoptStage(SmUfsMaps &map);
	void probeState59(SmUfsMaps &map);
	int  adoptMinimal(SmUfsMaps &map, bool samePage);
	int  adoptKebab(SmUfsMaps &map);
	int  adoptPure(SmUfsMaps &map);
	bool mapWindows(SmUfsMaps &map);
	void report(void);
	void report3(void);
	void report9(void);
	void report12(void);
	void report15(void);
	void reportPre21(uintptr_t hc, uintptr_t gcc);
	void productionReport(void);
};

OSDefineMetaClassAndStructors(NativeUfs, IOService)

IOService *NativeUfs::probe(IOService *provider, SInt32 *score)
{
	IOService *matched = IOService::probe(provider, score);
	if (matched == nullptr) {
		return nullptr;
	}
	if (score) {
		*score = 500;
	}
	return this;
}

bool NativeUfs::start(IOService *provider)
{
	if (!IOService::start(provider)) {
		return false;
	}

	uint32_t level = 0;
	PE_parse_boot_argn("native-ufs-probe", &level, sizeof(level));
	uint32_t rootMode = 0;
	if (PE_parse_boot_argn("native-ufs-root", &rootMode, sizeof(rootMode)) && rootMode) {
		// Production root service: the persistent read-only provider, not a probe.
		level = 71;
	}
	if (level == 0) {
		return false;
	}
	gSmUfs.level = level;

	if (!OSDynamicCast(IOPlatformExpertDevice, provider)) {
		// The ufshc nub match is observation-only; the probe is single-entry and starts from the
		// root device.
		SMUFS_LOG("ufshc node matched; probe runs from the root device");
		return false;
	}

	uint32_t delay = 10000;
	PE_parse_boot_argn("native-ufs-probe-delay", &delay, sizeof(delay));
	gSmUfs.delayMs = delay;

	SMUFS_LOG("probe level %u in %u ms", level, delay);
	thread_t thread = nullptr;
	if (level == 65 || level == 66 || level == 67 || level == 68 || level == 69 || level == 70 || level == 71) {
		blockStartupGate = IOLockAlloc();
		if (!blockStartupGate) return false;
		retain();
	}
	if (kernel_thread_start(probeThread, static_cast<void *>(this), &thread) != KERN_SUCCESS) {
		if (level == 65 || level == 66 || level == 67 || level == 68 || level == 69 || level == 70 || level == 71) release();
		SMUFS_LOG("cannot start the probe thread");
		return false;
	}
	thread_deallocate(thread);
	// The takeover touches MMIO the bootloader left running; if one step wedges, this thread still
	// reaches the panic screen and names the step.
	if (kernel_thread_start(watchdogThread, static_cast<void *>(this), &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
	return true;
}

// The framebuffer console only exists after the platform expert has published its resources, so
// the report has to wait for it; a panic before that prints nowhere and the board just resets.
void NativeUfs::probeThread(void *argument, wait_result_t)
{
	NativeUfs *self = static_cast<NativeUfs *>(argument);
	if (gSmUfs.level == 70) probe70Capture(0);
	IOSleep(gSmUfs.delayMs);
	self->runProbe(gSmUfs.level);
	if (gSmUfs.level == 71) {
		// root=1 retains the delayed diagnostic. root=2 may finish the healthy
		// observation while the provider stays online; end this kernel worker.
		self->productionReport();
		(void)thread_terminate(current_thread());
		panic("SMUFS root-h3p observer thread termination returned");
	}
	gSmUfs.progressDone = 1;
	if (gSmUfs.level == 9 || gSmUfs.level == 10 || gSmUfs.level == 11) {
		self->report9();
	}
	if (gSmUfs.level >= 3) {
		self->report3();
	}
	self->report();
}

void NativeUfs::watchdogThread(void *argument, wait_result_t)
{
	NativeUfs *self = static_cast<NativeUfs *>(argument);
	if (gSmUfs.level >= 44) {
		// The live-instrument levels spin forever by design and report through the squares;
		// the ring17 dump would only clear the frame the user is photographing.
		return;
	}
	// 1.5 s after the probe was due to start reporting: longer than any bounded poll in the probe,
	// and early enough to beat both the panel blank and the bus error a stalled transfer causes.
	IOSleep(gSmUfs.delayMs + (gSmUfs.level >= 25 ? 5000 : 1500));
	if (gSmUfs.progressDone == 0) {
		if (gSmUfs.level >= 17) {
			ringReportPanic();
		}
		if (gSmUfs.level >= 16) {
			// The first doorbell takes the UFS slave, and soon the interconnect, down with it; every
			// read in dumpHangState can wedge before the panic prints. Report only what is already
			// in memory, so a post-doorbell hang still names the operation that did not return.
			panic("SMUFS stuck16 phase=%x step=%x fb2 %x\n"
			      "dma %llx low%x cap %08x\n"
			      "clk %08x>%08x wild r%x s%x b%x\n"
			      "smr %08x s2cr %x\n",
			      gSmUfs.kbPhase, gSmUfs.kbStep, gSmUfs.kbFb2,
			      gSmUfs.kbDmaPa, gSmUfs.kbDmaLow, gSmUfs.kbCapReg,
			      gSmUfs.kbClkReqBefore, gSmUfs.kbClkReqAfter,
			      gSmUfs.wildResult, gSmUfs.wildSlot, gSmUfs.wildBank,
			      gSmUfs.wildSmr, gSmUfs.wildS2cr);
		}
		dumpHangState();
		panic("SMUFS stuck progress=%x take=%x stage=%x\n"
		      "cap %08x ver %08x hcs %08x hce %08x\n"
		      "rsr %08x hcs2 %08x dbr %08x dma %08x\n",
		      gSmUfs.progress, gSmUfs.takeResult, gSmUfs.stage,
		      gSmUfs.pre.cap, gSmUfs.pre.ver, gSmUfs.pre.hcs, gSmUfs.pre.hce,
		      gSmUfs.rsrBefore, gSmUfs.rsrAfter, gSmUfs.dbrAfter, gSmUfs.dmaPhys);
	}
	(void)self;
}

bool NativeUfs::mapWindows(SmUfsMaps &map)
{
	// GCC covers the UFS branches at 0x77000 and the PHY power domain at 0x177004.
	map.gcc = mapPhys(SM_GCC_BASE, 0x80000, map.gccMap);
	map.hc  = mapPhys(SM_UFS_HC_BASE, SM_UFS_HC_SIZE, map.hcMap);
	map.phy = mapPhys(SM_UFS_PHY_BASE, SM_UFS_PHY_SIZE, map.phyMap);
	gSmUfs.gccBase = map.gcc;
	gSmUfs.phyBase = map.phy;
	gSmUfs.hcBase = map.hc;
	return map.gcc && map.hc && map.phy;
}

void NativeUfs::runProbe(uint32_t level)
{
	SmUfsMaps map{};
	gSmUfs.stage = 1;
	if (!mapWindows(map)) {
		gSmUfs.result = 0x100;   // window mapping failed
		return;
	}

	// Put something on the panel before the first MMIO sweep. The adoption path reads the UFS
	// and PHY blocks before it powers anything, and those reads have no timeout: if one stalls,
	// this marker and line are the only evidence that the probe started at all.
	if (level >= 3) {
		uint64_t fbPhys = PE_state.video.v_baseAddr ? (uint64_t)PE_state.video.v_baseAddr : SM_FB_BASE;
		gFb.stride = PE_state.video.v_rowBytes ? (uint32_t)PE_state.video.v_rowBytes : SM_FB_STRIDE;
		gFb.base = mapPhys(fbPhys, 0xA00000, gFbMap);
		gFb.ok = gFb.base != 0;
		fbMark(0);
		kprintf("SMUFS enter v=%x fb=%llx stride=%x\n", level, fbPhys, gFb.stride);
	}

	snapshot(map, gSmUfs.pre);
	snapshotSmmu();
	gSmUfs.stage = 2;
	if (level < 2) {
		gSmUfs.post = gSmUfs.pre;
		return;
	}
	if (level == 59 || level == 60 || level == 61 || level == 62 || level == 63 || level == 64 || level == 65 || level == 66 || level == 67 || level == 68 || level == 69 || level == 70 || level == 71) {
		probeState59(map);
		return;
	}
	if (level >= 3) {
		int result = 0;
		if (level == 9) {
			result = adoptMinimal(map, false);
		} else if (level == 10 || level == 11 || level == 12 || level == 13 || level == 14 ||
		           level == 38 || level == 39 || level == 40 || level == 41 || level == 42 ||
		           level == 43 || level == 44 || level == 45 || level == 46 || level == 47 ||
		           level == 48 || level == 49 || level == 50 || level == 51 ||
		           level == 52 || level == 53 || level == 54 || level == 55 ||
		           level == 56 || level == 58) {
			result = adoptMinimal(map, true);
		} else if (level == 26) {
			result = adoptPure(map);
		} else if (level >= 15) {
			result = adoptKebab(map);
		} else {
			result = adoptStage(map);
		}
		gSmUfs.result = (uint32_t)result;
		snapshot(map, gSmUfs.post);
		return;
	}

	gSmUfs.result = (uint32_t)coldStart(map);
	gSmUfs.stage = gSmUfs.stage ? gSmUfs.stage : 2;
	snapshot(map, gSmUfs.post);
}

int NativeUfs::coldStart(SmUfsMaps &map)
{
	// Step 2: the PHY's power domain must be on before any of its registers hold state.
	if (powerGdsc(map.gcc) != 0) {
		gSmUfs.stage = 3;
		return 2;
	}
	gSmUfs.stage = 3;

	// Step 3: the nine UFS branches from gcc-kona.c.
	uint32_t failed = 0;
	failed |= (uint32_t)enableBranch(map.gcc, SM_GCC_UFS_AXI_CBCR) << 0;
	failed |= (uint32_t)enableBranch(map.gcc, SM_GCC_UFS_AHB_CBCR) << 1;
	failed |= (uint32_t)enableBranch(map.gcc, SM_GCC_UFS_UNIPRO_CORE_CBCR) << 2;
	failed |= (uint32_t)enableBranch(map.gcc, SM_GCC_AGGRE_UFS_AXI_CBCR) << 3;
	failed |= (uint32_t)enableBranch(map.gcc, SM_GCC_UFS_PHY_AUX_CBCR) << 4;
	failed |= (uint32_t)enableBranch(map.gcc, SM_GCC_UFS_ICE_CORE_CBCR) << 5;
	failed |= (uint32_t)enableBranch(map.gcc, SM_GCC_UFS_TX_SYMBOL0_CBCR) << 6;
	failed |= (uint32_t)enableBranch(map.gcc, SM_GCC_UFS_RX_SYMBOL0_CBCR) << 7;
	failed |= (uint32_t)enableBranch(map.gcc, SM_GCC_UFS_RX_SYMBOL1_CBCR) << 8;
	if (failed != 0) {
		gSmUfs.stage = 4;
		return 0x30 | failed;
	}
	gSmUfs.stage = 4;

	// Step 4: GCC_UFS_PHY_BCR pulse; the hardware requirement between assert and deassert is
	// three to four 32.7 kHz cycles, so 200 us is used as the vendor driver does.
	writeReg(map.gcc, SM_GCC_UFS_BCR, 1);
	IODelay(200);
	writeReg(map.gcc, SM_GCC_UFS_BCR, 0);
	IODelay(200);
	gSmUfs.stage = 5;

	// Step 5: the controller-side PHY soft reset that wraps calibration (REG_UFS_CFG1 bit 1).
	writeReg(map.hc, SM_UFS_REG_CFG1, readReg(map.hc, SM_UFS_REG_CFG1) | SM_UFS_CFG1_PHY_SOFT_RESET);
	(void)readReg(map.hc, SM_UFS_REG_CFG1);
	gSmUfs.stage = 6;

	// Step 6: PHY power control and calibration.
	phyPowerControl(map.phy);
	phyCalibrate(map.phy);
	writeReg(map.hc, SM_UFS_REG_CFG1, readReg(map.hc, SM_UFS_REG_CFG1) & ~SM_UFS_CFG1_PHY_SOFT_RESET);
	(void)readReg(map.hc, SM_UFS_REG_CFG1);
	IODelay(1000);   // the vendor driver waits 1 ms after PHY reset deassertion
	gSmUfs.stage = 7;

	// Step 7: SerDes start and the PCS poll (10 us up to 1 s in the vendor driver).
	writeReg(map.phy, SM_PHY_START, readReg(map.phy, SM_PHY_START) | SM_PHY_SERDES_START_BIT);
	__asm__ volatile("dsb sy" ::: "memory");
	gSmUfs.phyResult = 1;
	for (uint32_t i = 0; i < 1000; i++) {
		if (readReg(map.phy, SM_PHY_PCS_READY) & SM_PHY_PCS_READY_BIT) {
			gSmUfs.phyResult = 0;
			gSmUfs.pcsPolls = i;
			break;
		}
		IODelay(1000);
	}
	gSmUfs.stage = 8;
	if (gSmUfs.phyResult != 0) {
		return 0x50;
	}

	// Step 8: QUNIPRO select, the last action of ufs_qcom_power_up_sequence.
	writeReg(map.hc, SM_UFS_REG_CFG1, readReg(map.hc, SM_UFS_REG_CFG1) | SM_UFS_CFG1_QUNIPRO_SEL);
	gSmUfs.stage = 9;
	return 0;
}

// Level 3: the bootloader already brought the controller up (clocks, PHY, link and a request
// list), so adopt it. Allocate the DMA region, then let adoptAndIdentify replace the list and
// issue the identification commands.
int NativeUfs::adoptStage(SmUfsMaps &map)
{
	// Keep the UFS branch clocks running for the controller's own DMA. The bootloader leaves them
	// enabled but hardware-gated (CBCR bit31 CLK_OFF); a register access wakes the slave, while the
	// descriptor fetch can still wait on a gated clock and the doorbell then never completes.
	// Clearing the gating control bit (bit1) holds each branch on; docs/porting/ufs.md records the
	// "check enable and gating bits together" rule.
	uint32_t clk = readReg(map.gcc, SM_GCC_UFS_AXI_CBCR);
	writeReg(map.gcc, SM_GCC_UFS_AXI_CBCR, (clk | SM_GCC_BRANCH_ENABLE) & ~SM_GCC_BRANCH_HW_GATE);
	gSmUfs.clkAxi = readReg(map.gcc, SM_GCC_UFS_AXI_CBCR);
	clk = readReg(map.gcc, SM_GCC_AGGRE_UFS_AXI_CBCR);
	writeReg(map.gcc, SM_GCC_AGGRE_UFS_AXI_CBCR, (clk | SM_GCC_BRANCH_ENABLE) & ~SM_GCC_BRANCH_HW_GATE);
	gSmUfs.clkAgg = readReg(map.gcc, SM_GCC_AGGRE_UFS_AXI_CBCR);
	clk = readReg(map.gcc, SM_GCC_UFS_UNIPRO_CORE_CBCR);
	writeReg(map.gcc, SM_GCC_UFS_UNIPRO_CORE_CBCR, (clk | SM_GCC_BRANCH_ENABLE) & ~SM_GCC_BRANCH_HW_GATE);
	gSmUfs.clkUnip = readReg(map.gcc, SM_GCC_UFS_UNIPRO_CORE_CBCR);
	clk = readReg(map.gcc, SM_GCC_UFS_ICE_CORE_CBCR);
	writeReg(map.gcc, SM_GCC_UFS_ICE_CORE_CBCR, (clk | SM_GCC_BRANCH_ENABLE) & ~SM_GCC_BRANCH_HW_GATE);
	gSmUfs.clkIce = readReg(map.gcc, SM_GCC_UFS_ICE_CORE_CBCR);
	kprintf("SMUFS clk axi=%x agg=%x unip=%x ice=%x\n", gSmUfs.clkAxi, gSmUfs.clkAgg,
	        gSmUfs.clkUnip, gSmUfs.clkIce);

	// The branch registers only gate the leaves. Probe 6/7 additionally verifies the four clock
	// roots behind them, because a branch that reads enabled delivers nothing while its root is
	// stopped. Probe 4 keeps the old pre-ring report with no clock writes.
	if (gSmUfs.level >= 6) {
		gSmUfs.rcgForced = repairRcgRoots(map.gcc);
		reportRcgRoots();
	}

	gSmUfs.bypassResult = (uint32_t)inspectSmmu();
	uint64_t list = (uint64_t)readReg(map.hc, SM_UFS_REG_UTRLBA) |
	                ((uint64_t)readReg(map.hc, SM_UFS_REG_UTRLBAU) << 32);
	gSmUfs.dmaPhys = (uint32_t)list;
	kprintf("SMUFS list=%llx rsr=%x hcs=%x hce=%x\n", list, readReg(map.hc, SM_UFS_REG_UTRLRSR), readReg(map.hc, SM_UFS_REG_HCS), readReg(map.hc, SM_UFS_REG_HCE));
	fbMark(1);
	if (list == 0 || (list & 4095)) {
		return 0x70;
	}
	SmUfsDma dma = {};
	dma.phys = list;
	dma.virt = mapDma(list, kDmaBytes, dma.map);
	if (dma.virt == nullptr) {
		return 0x71;
	}
	fbMark(2);
	kprintf("SMUFS mapped virt=%p\n", dma.virt);
	zeroWords(dma.virt, kDmaBytes);
	cacheClean(dma.virt, kDmaBytes);
	gSmUfs.stage = 3;
	return adoptAndIdentify(map.hc, dma);
}

// Pre-ring transport state. The request fetch is the only step that never completes, so record
// the two things the controller itself cannot show: the NoC QoS block for the UFS master port
// (mas-xm-ufs-mem, cell-id 0x7B, qport 3 on aggre1_noc) and the link power mode the firmware
// left the device in. Both are read-only and safe before the first doorbell.
void reportTransportState(uintptr_t hc)
{
	OSSharedPtr<IOMemoryMap> nocMap;
	uintptr_t n = mapPhys(0x16e0000ULL, 0x20000, nocMap);
	if (n != 0) {
		// aggre1_noc base + base_offset 0x2000 + qos_off 0x1000 * qport 3
		gSmUfs.nocPrio = readReg(n, 0x5008);
		gSmUfs.nocMode = readReg(n, 0x500c);
		gSmUfs.nocBw = readReg(n, 0x5010);
		gSmUfs.nocSat = readReg(n, 0x5014);
	}
	gSmUfs.aggreCbcr = readReg(gSmUfs.gccBase, SM_GCC_AGGRE_UFS_AXI_CBCR);
	// Local DME_GET(PA_PWRMODE = 0x1571). No peer round trip: the value is the host's own view.
	writeReg(hc, SM_UFS_REG_IS, SM_UIC_COMMAND_COMPL);
	writeReg(hc, SM_UFS_REG_UICARG1, 0x1571U << 16);
	writeReg(hc, SM_UFS_REG_UICARG2, 0);
	writeReg(hc, SM_UFS_REG_UICARG3, 0);
	writeReg(hc, SM_UFS_REG_UICCMD, 0x01);
	bool done = false;
	for (unsigned loop = 0; loop < 10000; loop++) {
		if (readReg(hc, SM_UFS_REG_IS) & SM_UIC_COMMAND_COMPL) {
			done = true;
			break;
		}
		IODelay(10);
	}
	uint32_t result = readReg(hc, SM_UFS_REG_UICARG2) & 0xff;
	uint32_t value = readReg(hc, SM_UFS_REG_UICARG3);
	writeReg(hc, SM_UFS_REG_IS, SM_UIC_COMMAND_COMPL);
	gSmUfs.pwrDone = done ? 1 : 0;
	gSmUfs.pwrRes = result;
	gSmUfs.pwrMode = value;
	kprintf("SMUFS pwr dme_get done %x res %x pa_pwrmode %08x\n", done, result, value);
}

// The UFS host has no stream-match entry on this SMMU, so its AXI reads are unmatched. If it is
// behind the apps SMMU at all (the aggre1_noc port sits in the ANOC1 TBU's SID range), the
// firmware's USFCFG=1 makes those reads fault. ShengUfs solves the same handoff state for its
// known stream with a reserved bypass context bank; this board's SID is unknown, so the same
// mechanism is applied to a wildcard match over the ANOC1 range (0x000-0x3FF, mask 0x3FF). The
// display's SMR (0x820/0x402) lies outside that range and is not touched.
int smmuBypassAnoc1(void)
{
	OSSharedPtr<IOMemoryMap> map;
	uintptr_t s = mapPhys(0x15000000ULL, 0xd0000, map);
	if (s == 0) {
		return 1;
	}
	uint32_t idr0 = readReg(s, 0x20);
	uint32_t idr1 = readReg(s, 0x24);
	uint32_t idr2 = readReg(s, 0x28);
	if ((idr0 & (1U << 30)) == 0 || (idr2 & (1U << 12)) == 0) {
		return 2;
	}
	uint32_t count = idr0 & 0xff;
	uint32_t banks = idr1 & 0xff;
	uint32_t pages = 1U << (((idr1 >> 28) & 7) + 1);
	if ((idr1 >> 31) || count == 0 || count > 128 || pages > 128 || banks < 2 || banks > 128 ||
	    (pages + banks) * 4096 > 0xd0000) {
		return 3;
	}
	uint32_t bank = banks - 1;
	uintptr_t cb = s + (pages + bank) * 4096;
	if (readReg(cb, 0) & 1) {
		return 4;
	}
	uint32_t slot = 0xffffffffU;
	for (uint32_t index = 0; index < count; index++) {
		uint32_t smr = readReg(s, 0x800 + 4 * index);
		if ((smr & 0x80000000U) == 0) {
			if (slot == 0xffffffffU) {
				slot = index;
			}
			continue;
		}
		uint32_t s2cr = readReg(s, 0xc00 + 4 * index);
		if (((s2cr >> 16) & 3) == 0 && (s2cr & 0xff) == bank) {
			return 5;   // the reserved bank already carries another stream
		}
	}
	if (slot == 0xffffffffU) {
		return 6;
	}
	// configureUfsSmmu's order: disable the bank, set VA64, bypass S2, then S2CR before SMR.
	writeReg(cb, 0, 0);
	writeReg(s, 0x1800 + 4 * bank, 1);
	writeReg(s, 0x1000 + 4 * bank, 0x10000);
	writeReg(s, 0xc00 + 4 * slot, bank);
	writeReg(s, 0x800 + 4 * slot, 0x83FF0000U);   // VALID | mask 0x3FF | SID 0
	gSmUfs.wildSlot = slot;
	gSmUfs.wildBank = bank;
	gSmUfs.wildSmr = readReg(s, 0x800 + 4 * slot);
	gSmUfs.wildS2cr = readReg(s, 0xc00 + 4 * slot);
	gSmUfs.wildCbar = readReg(s, 0x1000 + 4 * bank);
	gSmUfs.wildCba2r = readReg(s, 0x1800 + 4 * bank);
	gSmUfs.wildSctlr = readReg(cb, 0);
	if (gSmUfs.wildSmr != 0x83FF0000U || gSmUfs.wildS2cr != bank ||
	    gSmUfs.wildCbar != 0x10000 || gSmUfs.wildCba2r != 1 || gSmUfs.wildSctlr != 0) {
		return 7;
	}
	return 0;
}

// Level 9/10: the ShengUfs adoption path, verbatim in order. ShengUfs/NativeUfs.cpp adoptAndRead
// and RootQueue.h take over a controller the firmware already enabled and linked: mask interrupts,
// stop the transfer list, point its base at the firmware list, acknowledge status, start the list,
// then submit. There is no HCE cycle, no DME_LINK_STARTUP, no ICE, no UTMRL takeover and no
// clock/timer programming; the reference explicitly says the controller is not reset and the link
// is not restarted. A kernel-allocated buffer is not reachable from this controller on the
// reference hardware either, so the 16 KiB at the firmware's UTRLBA is used as cached memory.
//
// Level 10 moves the UCD, response and PRDT into the first 4 KiB page of that list (level 9 keeps
// adoptAndRead's +0x1000 placement). docs/porting/ufs.md only guarantees that the firmware's
// request list is reachable; on this board we have no proof that the pages after it are, and the
// UTRD fetch is the only reference path step we have never seen complete.
// Defined with the other SMMU helpers below; adoptMinimal (level 38) calls it first.
static uint32_t smmuAllowStream(uint16_t stream, uint64_t dma, uint64_t bytes);

#include "Probe59Runner.h"

int NativeUfs::adoptMinimal(SmUfsMaps &map, bool samePage)
{
	uintptr_t hc = map.hc;
	uint64_t list = ((uint64_t)readReg(hc, SM_UFS_REG_UTRLBAU) << 32) |
	                readReg(hc, SM_UFS_REG_UTRLBA);
	gSmUfs.minList = list;
	gSmUfs.minStep = 1;
	if (list == 0 || (list & 0xfff)) {
		return 0x70;
	}
	gSmUfs.minHcsBefore = readReg(hc, SM_UFS_REG_HCS);
	gSmUfs.minHce = readReg(hc, SM_UFS_REG_HCE);
	gSmUfs.minIsBefore = readReg(hc, SM_UFS_REG_IS);
	gSmUfs.minDbrBefore = readReg(hc, SM_UFS_REG_UTRLDBR);
	gSmUfs.minTmrlBefore = readReg(hc, 0x78);
	gSmUfs.minRsrBefore = readReg(hc, SM_UFS_REG_UTRLRSR);
	gSmUfs.minFatal = gSmUfs.minIsBefore & 0x71880U;
	if ((gSmUfs.minHce & 1) == 0 || (gSmUfs.minHcsBefore & 0x0f) != 0x0f ||
	    gSmUfs.minDbrBefore != 0 || gSmUfs.minTmrlBefore != 0 || gSmUfs.minFatal != 0) {
		return 0x72;
	}
	fbMark2(1);
	SmUfsDma dma = {};
	dma.phys = list;
	dma.virt = mapDma(list, 16384, dma.map);
	if (dma.virt == nullptr) {
		return 0x71;
	}
	zeroWords(dma.virt, 16384);
	cacheClean(dma.virt, 16384);
	gSmUfs.minStep = 2;
	if (gSmUfs.level == 58) {
		// SET FLAG fDeviceInit (func 0x81) and wait for the device to self-clear it.
		// While the device initializes it holds the UTP layer busy - the queue stop below
		// would fail (the level-57 second-row-1 signature). The SMMU streams come first so
		// the query UTRD fetch does not fault.
		gBuildVa = dma.virt;
		gDmaPhys = dma.phys;
		OSSharedPtr<IOMemoryMap> respKeep58, utrdKeep58;
		gL46Resp = mapPhys(dma.phys + 0x40 + 512, 128, respKeep58);
		gL46Utrd = mapPhys(dma.phys, 64, utrdKeep58);
		const volatile uint8_t *rsp = reinterpret_cast<const volatile uint8_t *>(gL46Resp);
		gSmUfs.kbSmmuUfs = smmuAllowStream(0x0e0, dma.phys, 16384);
		if (gSmUfs.kbSmmuUfs == 1) {
			gSmUfs.kbSmmuStream2 = smmuAllowStream(0x4e0, dma.phys, 16384);
		}
		if (gSmUfs.kbSmmuUfs != 1 || gSmUfs.kbSmmuStream2 != 1) {
			panic("SMUFS l58 smmu: %x %x", gSmUfs.kbSmmuUfs, gSmUfs.kbSmmuStream2);
		}
		fbFill(gFb, 448, 896, 24, 24, 0xff00ff00U);
		for (unsigned st = 0; st < 20; st++) {
			gSettries = st + 1;
			zeroWords(dma.virt, 32);
			zeroWords(dma.virt + 0x40, 1040);
			(dma.virt + 0x40)[0] = 0x16;   // QUERY REQUEST
			(dma.virt + 0x40)[5] = 0x81;   // standard write request (0x81!)
			(dma.virt + 0x40)[12] = 0x06;  // opcode: SET FLAG
			(dma.virt + 0x40)[13] = 0x01;  // IDN: fDeviceInit
			writeUtrd(dma.virt, dma.phys, 0, false);
			cacheClean(dma.virt, 16384);
			writeReg(hc, SM_UFS_REG_IS, 0xffffffffU);
			writeReg(hc, SM_UFS_REG_UTRLDBR, 1);
			if (!pollDoorbellClear(hc)) {
				panic("SMUFS l58 setflag %u: doorbell never cleared", st);
			}
			for (unsigned i = 0; i < 16; i++) {
				gSetresp[i] = rsp[i];
			}
			if (rsp[6] == 0x00) {
				gSetok = true;
				fbFill(gFb, 512, 896, 24, 24, 0xff00ff00U);
				break;
			}
			fbFill(gFb, 16 + st * 6, 1712, 6, 24, 0xffff0000U);
			IODelay(100000);
		}
		if (gSetok) {
			fbFill(gFb, 448, 896, 24, 24, 0xff00ff00U);
		}
		bool finitDone = false;
		for (unsigned attempt = 0; attempt < 300; attempt++) {
			zeroWords(dma.virt, 32);
			zeroWords(dma.virt + 0x40, 1040);
			(dma.virt + 0x40)[0] = 0x16;   // QUERY REQUEST
			(dma.virt + 0x40)[5] = 0x01;   // standard read request
			(dma.virt + 0x40)[12] = 0x05;  // opcode: READ FLAG
			(dma.virt + 0x40)[13] = 0x01;  // IDN: fDeviceInit
			writeUtrd(dma.virt, dma.phys, 0, false);
			cacheClean(dma.virt, 16384);
			writeReg(hc, SM_UFS_REG_IS, 0xffffffffU);
			writeReg(hc, SM_UFS_REG_UTRLDBR, 1);
			if (!pollDoorbellClear(hc)) {
				break;
			}
			uint32_t value = ((uint32_t)rsp[20] << 24) | ((uint32_t)rsp[21] << 16) |
			                 ((uint32_t)rsp[22] << 8) | rsp[23];
			if (value == 0) {
				finitDone = true;
				fbFill(gFb, 576, 896, 24, 24, 0xff00ff00U);
				break;
			}
			if (attempt < 120) {
				fbFill(gFb, 16 + attempt * 6, 1616, 6, 24,
				       (attempt & 1) ? 0xffffffffU : 0xff00ff00U);
			}
			IODelay(200000);
		}
		if (!finitDone) {
			panic("SMUFS l58 finit: flag never cleared\n"
			      "rf  %08x %08x %08x %08x\n"
			      "rf+ %08x %08x %08x %08x",
			      pack32(rsp + 12), pack32(rsp + 16), pack32(rsp + 20),
			      pack32(rsp + 24), pack32(rsp + 28), pack32(rsp + 0),
			      pack32(rsp + 4), pack32(rsp + 8));
		}
	}
	writeReg(hc, 0x24, 0);   // mask interrupts, as hostStopQueue does
	writeReg(hc, SM_UFS_REG_UTRLRSR, 0);
	bool stopped = false;
	for (unsigned loop = 0; loop < 10000; loop++) {
		if ((readReg(hc, SM_UFS_REG_UTRLRSR) & 1) == 0) {
			stopped = true;
			break;
		}
		IODelay(10);
	}
	if (!stopped) {
		return 0x73;
	}
	fbMark2(2);
	gSmUfs.minStep = 3;
	if (gSmUfs.level >= 38 && gSmUfs.level <= 58) {
		// alioth (same SM8250) reaches its UTRD through a bypass context bank for
		// stream 0x60; without it the controller's fetch faults and the doorbell
		// never retires. The DMA range is the firmware's own list page.
		//
		// Level 39 replaces the SM8550 value with the streams this board's own IORT
		// assigns to \_SB.UFS0: 0x0e0 and 0x4e0, both through the SMMU at 0x15000000.
		// Level 38 wrote the SM8550 stream 0x60, so its SMR entry matched no master.
		const uint16_t stream = (gSmUfs.level == 38) ? 0x60 : 0x0e0;
		gSmUfs.kbSmmuUfs = smmuAllowStream(stream, list, 16384);
		if (gSmUfs.kbSmmuUfs != 1) {
			return 0x76;
		}
		if (gSmUfs.level >= 39) {
			// The IORT lists two mappings for UFS0; install both before ringing.
			gSmUfs.kbSmmuStream2 = smmuAllowStream(0x4e0, list, 16384);
			if (gSmUfs.kbSmmuStream2 != 1) {
				return 0x77;
			}
		}
	}
	writeReg(hc, SM_UFS_REG_UTRLBA, (uint32_t)list);
	writeReg(hc, SM_UFS_REG_UTRLBAU, (uint32_t)(list >> 32));
	if (readReg(hc, SM_UFS_REG_UTRLBA) != (uint32_t)list ||
	    readReg(hc, SM_UFS_REG_UTRLBAU) != (uint32_t)(list >> 32)) {
		return 0x74;
	}
	writeReg(hc, SM_UFS_REG_IS, 0xffffffffU);
	writeReg(hc, SM_UFS_REG_UTRLRSR, 1);
	bool ready = false;
	for (unsigned loop = 0; loop < 10000; loop++) {
		if ((readReg(hc, SM_UFS_REG_HCS) & 2) == 2) {
			ready = true;
			break;
		}
		IODelay(10);
	}
	gSmUfs.minRsrAfter = readReg(hc, SM_UFS_REG_UTRLRSR);
	gSmUfs.minHcsAfter = readReg(hc, SM_UFS_REG_HCS);
	if (!ready) {
		return 0x75;
	}
	fbMark2(3);
	if (gSmUfs.level == 42) {
		fbFill(gFb, 16, 896, 24, 24, 0xffffffffU);
		OSSharedPtr<IOMemoryMap> smmuMap;
		uintptr_t s = mapPhys(0x15000000ULL, 0xd0000, smmuMap);
		uint32_t smr0 = 0, smr1 = 0;
		if (s != 0) {
			for (unsigned i = 0; i < 128; i++) {
				uint32_t v = readReg(s, 0x800 + 4 * i);
				if (v == (0x80000000U | 0x0e0U)) {
					smr0 = v;
				} else if (v == (0x80000000U | 0x4e0U)) {
					smr1 = v;
				}
			}
		}
		fbFill(gFb, 64, 896, 24, 24, smr0 ? 0xff00ff00U : 0xffff0000U);
		fbFill(gFb, 96, 896, 24, 24, smr1 ? 0xff00ff00U : 0xffff0000U);
		fbFill(gFb, 128, 896, 24, 24, 0xff00ffffU);
		for (unsigned col = 160; col <= 384; col += 32) {
			fbFill(gFb, col, 896, 24, 24, 0xffffff00U);
		}
		gL41Smmu = s;
		gL41Entered = 0;
		gL41Done = 0;
		gL41Ticks = 0;
		gL41Gfsr = 0;
		gL41Syn0 = 0;
		gL41Syn1 = 0;
		gRingHc = hc;
		thread_t watchHandle = nullptr;
		thread_t ringHandle = nullptr;
		bool watching = gL41Smmu != 0 &&
		                kernel_thread_start(l41Watcher, nullptr, &watchHandle) == KERN_SUCCESS;
		if (watching) {
			thread_deallocate(watchHandle);
		}
		bool ringing = kernel_thread_start(l41Ring, nullptr, &ringHandle) == KERN_SUCCESS;
		if (ringing) {
			thread_deallocate(ringHandle);
		}
		for (unsigned repaint = 0; repaint < 600; repaint++) {
			uint32_t sid = gL41Syn1 & 0xfffU;
			fbFill(gFb, 160, 896, 24, 24, gL41Entered ? 0xff00ff00U : 0xffff0000U);
			fbFill(gFb, 192, 896, 24, 24, gL41Done ? 0xff00ff00U : 0xffff0000U);
			fbFill(gFb, 224, 896, 24, 24, gL41Ticks >= 50 ? 0xff00ff00U : 0xffff0000U);
			fbFill(gFb, 256, 896, 24, 24, gL41Gfsr ? 0xff00ff00U : 0xffff0000U);
			fbFill(gFb, 288, 896, 24, 24, sid == 0x0e0U ? 0xff00ff00U : 0xffff0000U);
			fbFill(gFb, 320, 896, 24, 24, sid == 0x4e0U ? 0xff00ff00U : 0xffff0000U);
			fbFill(gFb, 352, 896, 24, 24,
			       (sid && sid != 0x0e0U && sid != 0x4e0U) ? 0xff00ff00U : 0xffff0000U);
			fbFill(gFb, 384, 896, 24, 24, watching ? 0xff00ff00U : 0xffff0000U);
			if (repaint < 120) {
				fbFill(gFb, 16 + repaint * 6, 944, 6, 24,
				       (repaint & 1) ? 0xffffffffU : 0xff00ff00U);
			}
			IODelay(250000);
		}
		for (;;) {
			IODelay(1000000);
		}
	}
	if (gSmUfs.level == 43) {
		fbFill(gFb, 16, 896, 24, 24, 0xffffffffU);
		OSSharedPtr<IOMemoryMap> smmuMap;
		uintptr_t s = mapPhys(0x15000000ULL, 0xd0000, smmuMap);
		uint32_t smr0 = 0, smr1 = 0;
		if (s != 0) {
			for (unsigned i = 0; i < 128; i++) {
				uint32_t v = readReg(s, 0x800 + 4 * i);
				if (v == (0x80000000U | 0x0e0U)) {
					smr0 = v;
				} else if (v == (0x80000000U | 0x4e0U)) {
					smr1 = v;
				}
			}
		}
		fbFill(gFb, 64, 896, 24, 24, smr0 ? 0xff00ff00U : 0xffff0000U);
		fbFill(gFb, 96, 896, 24, 24, smr1 ? 0xff00ff00U : 0xffff0000U);
		fbFill(gFb, 128, 896, 24, 24, 0xff00ffffU);
		for (unsigned col = 160; col <= 384; col += 32) {
			fbFill(gFb, col, 896, 24, 24, 0xffffff00U);
		}
		gL41Smmu = s;
		gL41Entered = 0;
		gL41Done = 0;
		gL41Ticks = 0;
		gL41Gfsr = 0;
		gL41Syn0 = 0;
		gL41Syn1 = 0;
		gRingHc = hc;
		thread_t watchHandle = nullptr;
		thread_t ringHandle = nullptr;
		bool watching = gL41Smmu != 0 &&
		                kernel_thread_start(l45WatcherA, nullptr, &watchHandle) == KERN_SUCCESS;
		if (!watching) {
			fbFill(gFb, 384, 896, 24, 24, 0xffff0000U);
		} else {
			thread_deallocate(watchHandle);
		}
		bool ringing = kernel_thread_start(l43Ring, nullptr, &ringHandle) == KERN_SUCCESS;
		if (!ringing) {
			fbFill(gFb, 160, 896, 24, 24, 0xffff0000U);
		} else {
			thread_deallocate(ringHandle);
		}
		for (;;) {
			IODelay(1000000);
		}
	}
	if (gSmUfs.level == 44) {
		// Level 43 proved the doorbell store retires once streams 0x0e0/0x4e0 are mapped: the
		// ring thread greened x=160 before the store and x=192 after it. The freeze within
		// 50 ms afterwards matches the controller fetching the all-zero UTRD (CT=0h is not a
		// valid command). Level 44 rings the SAME prepared NOP OUT the min15 path builds, so
		// the controller has a real command to execute; the watcher now also polls the host
		// and greens x=416 the moment the doorbell clears and x=448 when IS shows any event.
		fbFill(gFb, 16, 896, 24, 24, 0xffffffffU);
		OSSharedPtr<IOMemoryMap> smmuMap;
		uintptr_t s = mapPhys(0x15000000ULL, 0xd0000, smmuMap);
		uint32_t smr0 = 0, smr1 = 0;
		if (s != 0) {
			for (unsigned i = 0; i < 128; i++) {
				uint32_t v = readReg(s, 0x800 + 4 * i);
				if (v == (0x80000000U | 0x0e0U)) {
					smr0 = v;
				} else if (v == (0x80000000U | 0x4e0U)) {
					smr1 = v;
				}
			}
		}
		fbFill(gFb, 64, 896, 24, 24, smr0 ? 0xff00ff00U : 0xffff0000U);
		fbFill(gFb, 96, 896, 24, 24, smr1 ? 0xff00ff00U : 0xffff0000U);
		fbFill(gFb, 128, 896, 24, 24, 0xff00ffffU);
		for (unsigned col = 160; col <= 448; col += 32) {
			fbFill(gFb, col, 896, 24, 24, 0xffffff00U);
		}
		enum : uint32_t { kMinRsp = 512, kMinPrdt = 1024 };
		uint8_t *utrd = dma.virt;
		uint8_t *ucd = dma.virt + 0x40U;
		zeroWords(utrd, 32);
		zeroWords(ucd, 1024 + 16);
		ucd[0] = 0x00;   // NOP OUT
		ucd[3] = 1;      // task tag
		uint64_t ucdPhys = list + 0x40U;
		*reinterpret_cast<uint32_t *>(utrd + 0) = 0x10000000U;
		*reinterpret_cast<uint32_t *>(utrd + 8) = 0x0fU;
		*reinterpret_cast<uint32_t *>(utrd + 16) = (uint32_t)ucdPhys;
		*reinterpret_cast<uint32_t *>(utrd + 20) = (uint32_t)(ucdPhys >> 32);
		*reinterpret_cast<uint32_t *>(utrd + 24) = (512U >> 2) | ((kMinRsp >> 2) << 16);
		*reinterpret_cast<uint32_t *>(utrd + 28) = (kMinPrdt >> 2) << 16;
		cacheClean(dma.virt, 16384);
		gL41Smmu = s;
		gL41Entered = 0;
		gL41Done = 0;
		gL41Ticks = 0;
		gL41Gfsr = 0;
		gL41Syn0 = 0;
		gL41Syn1 = 0;
		gRingHc = hc;
		thread_t watchHandle = nullptr;
		thread_t hostHandle = nullptr;
		thread_t ringHandle = nullptr;
		bool watching = gL41Smmu != 0 &&
		                kernel_thread_start(l45WatcherA, nullptr, &watchHandle) == KERN_SUCCESS;
		if (!watching) {
			fbFill(gFb, 384, 896, 24, 24, 0xffff0000U);
		} else {
			thread_deallocate(watchHandle);
		}
		bool polling = kernel_thread_start(l45WatcherB, nullptr, &hostHandle) == KERN_SUCCESS;
		if (!polling) {
			fbFill(gFb, 416, 896, 24, 24, 0xffff0000U);
		} else {
			thread_deallocate(hostHandle);
		}
		bool ringing = kernel_thread_start(l43Ring, nullptr, &ringHandle) == KERN_SUCCESS;
		if (!ringing) {
			fbFill(gFb, 160, 896, 24, 24, 0xffff0000U);
		} else {
			thread_deallocate(ringHandle);
		}
		for (;;) {
			IODelay(1000000);
		}
	}
	if (gSmUfs.level == 45) {
		// Same prepared NOP OUT ring as 44; the observers are split per port so the frozen
		// frame shows which interconnect port died and what the SMMU had recorded by then.
		fbFill(gFb, 16, 896, 24, 24, 0xffffffffU);
		OSSharedPtr<IOMemoryMap> smmuMap;
		uintptr_t s = mapPhys(0x15000000ULL, 0xd0000, smmuMap);
		uint32_t smr0 = 0, smr1 = 0;
		if (s != 0) {
			for (unsigned i = 0; i < 128; i++) {
				uint32_t v = readReg(s, 0x800 + 4 * i);
				if (v == (0x80000000U | 0x0e0U)) {
					smr0 = v;
				} else if (v == (0x80000000U | 0x4e0U)) {
					smr1 = v;
				}
			}
		}
		fbFill(gFb, 64, 896, 24, 24, smr0 ? 0xff00ff00U : 0xffff0000U);
		fbFill(gFb, 96, 896, 24, 24, smr1 ? 0xff00ff00U : 0xffff0000U);
		fbFill(gFb, 128, 896, 24, 24, 0xff00ffffU);
		for (unsigned col = 160; col <= 480; col += 32) {
			fbFill(gFb, col, 896, 24, 24, 0xffffff00U);
		}
		enum : uint32_t { kMinRsp = 512, kMinPrdt = 1024 };
		uint8_t *utrd = dma.virt;
		uint8_t *ucd = dma.virt + 0x40U;
		zeroWords(utrd, 32);
		zeroWords(ucd, 1024 + 16);
		ucd[0] = 0x00;   // NOP OUT
		ucd[3] = 1;      // task tag
		uint64_t ucdPhys = list + 0x40U;
		*reinterpret_cast<uint32_t *>(utrd + 0) = 0x10000000U;
		*reinterpret_cast<uint32_t *>(utrd + 8) = 0x0fU;
		*reinterpret_cast<uint32_t *>(utrd + 16) = (uint32_t)ucdPhys;
		*reinterpret_cast<uint32_t *>(utrd + 20) = (uint32_t)(ucdPhys >> 32);
		*reinterpret_cast<uint32_t *>(utrd + 24) = (512U >> 2) | ((kMinRsp >> 2) << 16);
		*reinterpret_cast<uint32_t *>(utrd + 28) = (kMinPrdt >> 2) << 16;
		cacheClean(dma.virt, 16384);
		gL41Smmu = s;
		gL41Entered = 0;
		gL41Done = 0;
		gL41Ticks = 0;
		gL41Gfsr = 0;
		gL41Syn0 = 0;
		gL41Syn1 = 0;
		gRingHc = hc;
		thread_t watchHandle = nullptr;
		thread_t hostHandle = nullptr;
		thread_t ringHandle = nullptr;
		bool watching = gL41Smmu != 0 &&
		                kernel_thread_start(l45WatcherA, nullptr, &watchHandle) == KERN_SUCCESS;
		if (!watching) {
			fbFill(gFb, 384, 896, 24, 24, 0xffff0000U);
		} else {
			thread_deallocate(watchHandle);
		}
		bool polling = kernel_thread_start(l45WatcherB, nullptr, &hostHandle) == KERN_SUCCESS;
		if (!polling) {
			fbFill(gFb, 416, 896, 24, 24, 0xffff0000U);
		} else {
			thread_deallocate(hostHandle);
		}
		bool ringing = kernel_thread_start(l43Ring, nullptr, &ringHandle) == KERN_SUCCESS;
		if (!ringing) {
			fbFill(gFb, 160, 896, 24, 24, 0xffff0000U);
		} else {
			thread_deallocate(ringHandle);
		}
		for (unsigned repaint = 0; repaint < 120; repaint++) {
			fbFill(gFb, 16 + repaint * 6, 992, 6, 24,
			       (repaint & 1) ? 0xffffffffU : 0xff00ff00U);
			IODelay(250000);
		}
		for (;;) {
			IODelay(1000000);
		}
	}
	if (gSmUfs.level == 46) {
		// Level 45 proved the transport end to end: entered + returned green, the doorbell
		// cleared, no SMMU fault and both survival strips grew without stopping. Level 46
		// sends the first real SCSI command through the same path: TEST UNIT READY (6-byte
		// all-zero CDB, no data phase). The device is expected to answer CHECK CONDITION
		// (sense key 2, ASC 04) because fDeviceInit has never been sent; either answer
		// proves the device round trip. The response bytes are mirrored as bit rows.
		fbFill(gFb, 16, 896, 24, 24, 0xffffffffU);
		OSSharedPtr<IOMemoryMap> smmuMap;
		uintptr_t s = mapPhys(0x15000000ULL, 0xd0000, smmuMap);
		uint32_t smr0 = 0, smr1 = 0;
		if (s != 0) {
			for (unsigned i = 0; i < 128; i++) {
				uint32_t v = readReg(s, 0x800 + 4 * i);
				if (v == (0x80000000U | 0x0e0U)) {
					smr0 = v;
				} else if (v == (0x80000000U | 0x4e0U)) {
					smr1 = v;
				}
			}
		}
		fbFill(gFb, 64, 896, 24, 24, smr0 ? 0xff00ff00U : 0xffff0000U);
		fbFill(gFb, 96, 896, 24, 24, smr1 ? 0xff00ff00U : 0xffff0000U);
		fbFill(gFb, 128, 896, 24, 24, 0xff00ffffU);
		for (unsigned col = 160; col <= 576; col += 32) {
			fbFill(gFb, col, 896, 24, 24, 0xffffff00U);
		}
		enum : uint32_t { kMinRsp = 512, kMinPrdt = 1024 };
		uint8_t *utrd = dma.virt;
		uint8_t *ucd = dma.virt + 0x40U;
		zeroWords(utrd, 32);
		zeroWords(ucd, 1024 + 16);
		ucd[0] = 0x01;   // COMMAND UPIU
		ucd[1] = 0x00;   // flags: no data phase
		ucd[2] = 0x00;   // LUN 0
		ucd[3] = 0x00;   // task tag 0
		*reinterpret_cast<volatile uint8_t *>(ucd + 12) = 0;
		*reinterpret_cast<volatile uint8_t *>(ucd + 13) = 0;
		*reinterpret_cast<volatile uint8_t *>(ucd + 14) = 0;
		*reinterpret_cast<volatile uint8_t *>(ucd + 15) = 0;
		for (unsigned index = 0; index < 6; index++) {
			ucd[16 + index] = 0;   // TEST UNIT READY CDB
		}
		uint64_t ucdPhys = list + 0x40U;
		*reinterpret_cast<uint32_t *>(utrd + 0) = 0x10000000U;
		*reinterpret_cast<uint32_t *>(utrd + 8) = 0x0fU;
		*reinterpret_cast<uint32_t *>(utrd + 16) = (uint32_t)ucdPhys;
		*reinterpret_cast<uint32_t *>(utrd + 20) = (uint32_t)(ucdPhys >> 32);
		*reinterpret_cast<uint32_t *>(utrd + 24) = (512U >> 2) | ((kMinRsp >> 2) << 16);
		*reinterpret_cast<uint32_t *>(utrd + 28) = (kMinPrdt >> 2) << 16;
		cacheClean(dma.virt, 16384);
		gL41Smmu = s;
		gL41Entered = 0;
		gL41Done = 0;
		gL41Ticks = 0;
		gL41Gfsr = 0;
		gL41Syn0 = 0;
		gL41Syn1 = 0;
		gRingHc = hc;
		OSSharedPtr<IOMemoryMap> respKeep;
		OSSharedPtr<IOMemoryMap> utrdKeep;
		gL46Resp = mapPhys(dma.phys + 0x40 + 512, 128, respKeep);
		gL46Utrd = mapPhys(dma.phys, 64, utrdKeep);
		thread_t watchHandle = nullptr;
		thread_t hostHandle = nullptr;
		thread_t ringHandle = nullptr;
		bool watching = gL41Smmu != 0 &&
		                kernel_thread_start(l45WatcherA, nullptr, &watchHandle) == KERN_SUCCESS;
		if (!watching) {
			fbFill(gFb, 384, 896, 24, 24, 0xffff0000U);
		} else {
			thread_deallocate(watchHandle);
		}
		bool polling = kernel_thread_start(l46WatcherB, nullptr, &hostHandle) == KERN_SUCCESS;
		if (!polling) {
			fbFill(gFb, 416, 896, 24, 24, 0xffff0000U);
		} else {
			thread_deallocate(hostHandle);
		}
		bool ringing = kernel_thread_start(l43Ring, nullptr, &ringHandle) == KERN_SUCCESS;
		if (!ringing) {
			fbFill(gFb, 160, 896, 24, 24, 0xffff0000U);
		} else {
			thread_deallocate(ringHandle);
		}
		for (unsigned repaint = 0; repaint < 120; repaint++) {
			fbFill(gFb, 16 + repaint * 6, 992, 6, 24,
			       (repaint & 1) ? 0xffffffffU : 0xff00ff00U);
			IODelay(250000);
		}
		for (;;) {
			IODelay(1000000);
		}
	}
	if (gSmUfs.level == 47 || gSmUfs.level == 48 || gSmUfs.level == 49 ||
	    gSmUfs.level == 50 || gSmUfs.level == 51 || gSmUfs.level == 52 ||
	    gSmUfs.level == 53 || gSmUfs.level == 54 || gSmUfs.level == 55 ||
	    gSmUfs.level == 56 || gSmUfs.level == 58) {
		// The sequencer level: SET FLAG fDeviceInit -> poll READ FLAG -> TUR -> READ
		// CAPACITY(10). Everything is bounded and painted, so the frozen frame is the log.
		fbFill(gFb, 16, 896, 24, 24, 0xffffffffU);
		OSSharedPtr<IOMemoryMap> smmuMap;
		uintptr_t s = mapPhys(0x15000000ULL, 0xd0000, smmuMap);
		uint32_t smr0 = 0, smr1 = 0;
		if (s != 0) {
			for (unsigned i = 0; i < 128; i++) {
				uint32_t v = readReg(s, 0x800 + 4 * i);
				if (v == (0x80000000U | 0x0e0U)) {
					smr0 = v;
				} else if (v == (0x80000000U | 0x4e0U)) {
					smr1 = v;
				}
			}
		}
		fbFill(gFb, 64, 896, 24, 24, smr0 ? 0xff00ff00U : 0xffff0000U);
		fbFill(gFb, 96, 896, 24, 24, smr1 ? 0xff00ff00U : 0xffff0000U);
		fbFill(gFb, 128, 896, 24, 24, 0xff00ffffU);
		for (unsigned col = 160; col <= 672; col += 32) {
			fbFill(gFb, col, 896, 24, 24, 0xffffff00U);
		}
		OSSharedPtr<IOMemoryMap> respKeep;
		OSSharedPtr<IOMemoryMap> utrdKeep;
		OSSharedPtr<IOMemoryMap> capKeep;
		gL46Resp = mapPhys(dma.phys + 0x40 + 512, 128, respKeep);
		gL46Utrd = mapPhys(dma.phys, 64, utrdKeep);
		gCapVa = mapPhys(dma.phys + 12288, 64, capKeep);
		gBuildVa = dma.virt;
		gDmaPhys = dma.phys;
		gL41Entered = 0;
		gL41Done = 0;
		gL41Ticks = 0;
		gL41Gfsr = 0;
		gL41Syn0 = 0;
		gL41Syn1 = 0;
		gRingHc = hc;
		thread_t watchHandle = nullptr;
		thread_t seqHandle = nullptr;
		bool watching = false;
		if (gSmUfs.level != 49 && gL41Smmu != 0) {
			watching = kernel_thread_start(l45WatcherA, nullptr, &watchHandle) == KERN_SUCCESS;
			if (!watching) {
				IODelay(100000);
				watching = kernel_thread_start(l45WatcherA, nullptr, &watchHandle) == KERN_SUCCESS;
			}
		}
		if (!watching && gSmUfs.level != 49) {
			fbFill(gFb, 384, 896, 24, 24, 0xffff0000U);
		}
		if (watching) {
			thread_deallocate(watchHandle);
		}
		bool sequencing = kernel_thread_start(
		    gSmUfs.level == 47 ? l47Worker :
		    (gSmUfs.level == 48 || gSmUfs.level == 49) ? l48Worker :
		    (gSmUfs.level == 50) ? l50Worker :
		    (gSmUfs.level == 51) ? l51Worker :
		    (gSmUfs.level == 52) ? l52Worker :
		    (gSmUfs.level == 53) ? l53Worker :
		    (gSmUfs.level >= 54 && gSmUfs.level <= 56) ? l52Worker :
		    (gSmUfs.level == 57) ? l57Worker :
		    (gSmUfs.level == 58) ? l58Worker : l54Worker,
		    nullptr, &seqHandle) == KERN_SUCCESS;
		if (!sequencing) {
			fbFill(gFb, 160, 896, 24, 24, 0xffff0000U);
		} else {
			thread_deallocate(seqHandle);
		}
		for (unsigned repaint = 0; repaint < 120; repaint++) {
			fbFill(gFb, 16 + repaint * 6, 992, 6, 24,
			       (repaint & 1) ? 0xffffffffU : 0xff00ff00U);
			IODelay(250000);
		}
		for (;;) {
			IODelay(1000000);
		}
	}
	reportTransportState(hc);
	if (gSmUfs.level == 12) {
		report12();
	}
	if (gSmUfs.level == 13) {
		// Ring from a helper thread so a stalled store only wedges that thread, wait 20 ms, then
		// report on the panic channel (kprintf has not been reliable in these runs). Only the
		// SMMU is read: a fault there records the stream ID the firmware left unmatched, which
		// decides between SMMU and XPU/BCM.
		gRingHc = hc;
		gRingDone = 0;
		thread_t ringHandle = nullptr;
		bool started = kernel_thread_start(ringThread, nullptr, &ringHandle) == KERN_SUCCESS;
		if (started) {
			thread_deallocate(ringHandle);
		}
		for (unsigned loop = 0; loop < 20; loop++) {
			IODelay(1000);
			if (gRingDone) {
				break;
			}
		}
		OSSharedPtr<IOMemoryMap> smmuMap;
		uintptr_t s = mapPhys(0x15000000ULL, 0xd0000, smmuMap);
		uint32_t gfsr = s ? readReg(s, 0x48) : 0xdeadbeefU;
		uint32_t syn0 = s ? readReg(s, 0x50) : 0;
		uint32_t syn1 = s ? readReg(s, 0x54) : 0;
		uint32_t s2cr0 = s ? readReg(s, 0xc00) : 0;
		uint32_t scr0 = s ? readReg(s, 0x0) : 0;
		panic("SMUFS ring13 done=%x gfsr %08x syn %08x %08x s2cr0 %08x scr0 %08x",
		      gRingDone, gfsr, syn0, syn1, s2cr0, scr0);
	}
	if (gSmUfs.level == 39) {
		// Ring from a helper thread so a stalled store only wedges that thread. This CPU then
		// reads the SMMU's global fault status and both SMR entries, which separates "the
		// controller never fetched" from "the fetch was translated away". UFS registers are
		// only read when the store retired: reading a wedged block would hang this thread too.
		gRingHc = hc;
		gRingDone = 0;
		thread_t ringHandle = nullptr;
		bool started = kernel_thread_start(ringThread, nullptr, &ringHandle) == KERN_SUCCESS;
		if (started) {
			thread_deallocate(ringHandle);
		}
		for (unsigned loop = 0; loop < 50; loop++) {
			IODelay(1000);
			if (gRingDone) {
				break;
			}
		}
		OSSharedPtr<IOMemoryMap> smmuMap;
		uintptr_t s = mapPhys(0x15000000ULL, 0xd0000, smmuMap);
		uint32_t gfsr = s ? readReg(s, 0x48) : 0xdeadbeefU;
		uint32_t syn0 = s ? readReg(s, 0x50) : 0;
		uint32_t syn1 = s ? readReg(s, 0x54) : 0;
		uint32_t scr0 = s ? readReg(s, 0x0) : 0;
		uint32_t smr0 = 0, smr1 = 0, s2cr0 = 0, s2cr1 = 0;
		if (s) {
			for (unsigned i = 0; i < 128; i++) {
				uint32_t v = readReg(s, 0x800 + 4 * i);
				if (v == (0x80000000U | 0x0e0U)) {
					smr0 = v;
					s2cr0 = readReg(s, 0xc00 + 4 * i);
				} else if (v == (0x80000000U | 0x4e0U)) {
					smr1 = v;
					s2cr1 = readReg(s, 0xc00 + 4 * i);
				}
			}
		}
		uint32_t hcs = gRingDone ? readReg(hc, SM_UFS_REG_HCS) : 0xffffffffU;
		uint32_t isr = gRingDone ? readReg(hc, SM_UFS_REG_IS) : 0xffffffffU;
		uint32_t dbr = gRingDone ? readReg(hc, SM_UFS_REG_UTRLDBR) : 0xffffffffU;
		panic("SMUFS l39 done=%x gfsr %08x syn %08x %08x scr0 %08x smr %08x/%08x s2cr %08x/%08x hcs %08x is %08x dbr %08x",
		      gRingDone, gfsr, syn0, syn1, scr0, smr0, smr1, s2cr0, s2cr1, hcs, isr, dbr);
	}
	if (gSmUfs.level == 40) {
		// Colour-only report. The level-39 run reached this point (row 2 = 3, so both SMRs
		// read back) and then died silently: its post-ring SMMU read hung on a wedged bus
		// before panic() could print. Nothing here touches MMIO after the ring. Every value
		// is painted first, so a frozen frame still carries the answer: the blue square means
		// "about to ring", the square beside it proves whether the store returned.
		OSSharedPtr<IOMemoryMap> smmuMap;
		uintptr_t s = mapPhys(0x15000000ULL, 0xd0000, smmuMap);
		uint32_t smr0 = 0, smr1 = 0;
		if (s != 0) {
			for (unsigned i = 0; i < 128; i++) {
				uint32_t v = readReg(s, 0x800 + 4 * i);
				if (v == (0x80000000U | 0x0e0U)) {
					smr0 = v;
				} else if (v == (0x80000000U | 0x4e0U)) {
					smr1 = v;
				}
			}
		}
		fbFill(gFb, 16, 896, 24, 24, 0xffffffffU);
		fbFill(gFb, 64, 896, 24, 24, smr0 ? 0xff00ff00U : 0xffff0000U);
		fbFill(gFb, 96, 896, 24, 24, smr1 ? 0xff00ff00U : 0xffff0000U);
		fbFill(gFb, 128, 896, 24, 24, 0xff00ffffU);
		gRingHc = hc;
		gRingDone = 0;
		thread_t ringHandle = nullptr;
		bool started = kernel_thread_start(ringThread, nullptr, &ringHandle) == KERN_SUCCESS;
		if (started) {
			thread_deallocate(ringHandle);
		}
		for (unsigned loop = 0; loop < 100; loop++) {
			IODelay(1000);
			if (gRingDone) {
				break;
			}
		}
		fbFill(gFb, 160, 896, 24, 24, gRingDone ? 0xff00ff00U : 0xffff0000U);
		for (;;) {
			IODelay(1000000);
		}
	}
	if (gSmUfs.level == 41) {
		// Eight squares at y=896, painted once, in order: mapping proof / ring thread entered /
		// store returned / watcher kept running / SMMU recorded a fault / fault SID is 0x0e0 /
		// fault SID is 0x4e0 / fault SID is something else / watcher thread started.
		// A square that never appears means this CPU stopped before reaching it.
		fbFill(gFb, 16, 896, 24, 24, 0xffffffffU);
		OSSharedPtr<IOMemoryMap> smmuMap;
		gL41Smmu = mapPhys(0x15000000ULL, 0xd0000, smmuMap);
		gL41Entered = 0;
		gL41Done = 0;
		gL41Ticks = 0;
		gL41Gfsr = 0;
		gL41Syn0 = 0;
		gL41Syn1 = 0;
		gRingHc = hc;
		thread_t watchHandle = nullptr;
		thread_t ringHandle = nullptr;
		bool watching = gL41Smmu != 0 &&
		                kernel_thread_start(l41Watcher, nullptr, &watchHandle) == KERN_SUCCESS;
		if (watching) {
			thread_deallocate(watchHandle);
		}
		bool ringing = kernel_thread_start(l41Ring, nullptr, &ringHandle) == KERN_SUCCESS;
		if (ringing) {
			thread_deallocate(ringHandle);
		}
		IODelay(150000);
		uint32_t sid = gL41Syn1 & 0xfffU;
		fbFill(gFb, 64, 896, 24, 24, gL41Entered ? 0xff00ff00U : 0xffff0000U);
		fbFill(gFb, 96, 896, 24, 24, gL41Done ? 0xff00ff00U : 0xffff0000U);
		fbFill(gFb, 128, 896, 24, 24, gL41Ticks >= 50 ? 0xff00ff00U : 0xffff0000U);
		fbFill(gFb, 160, 896, 24, 24, gL41Gfsr ? 0xff00ff00U : 0xffff0000U);
		fbFill(gFb, 192, 896, 24, 24, sid == 0x0e0U ? 0xff00ff00U : 0xffff0000U);
		fbFill(gFb, 224, 896, 24, 24, sid == 0x4e0U ? 0xff00ff00U : 0xffff0000U);
		fbFill(gFb, 256, 896, 24, 24,
		       (sid && sid != 0x0e0U && sid != 0x4e0U) ? 0xff00ff00U : 0xffff0000U);
		fbFill(gFb, 288, 896, 24, 24, watching ? 0xff00ff00U : 0xffff0000U);
		for (;;) {
			IODelay(1000000);
		}
	}
	if (gSmUfs.level == 14) {
		gSmUfs.wildResult = (uint32_t)smmuBypassAnoc1();
	}
	gSmUfs.minStep = 4;
	// adoptAndRead's layout for the firmware list: [0,4096) list, [4096,5120) command + response
	// UPIU + PRDT, [8192,16384) data. Level 10 moves the whole command area into the first page.
	enum : uint32_t { kMinRsp = 512, kMinPrdt = 1024 };
	uint32_t ucdOffset = samePage ? 0x40U : 4096U;
	uint8_t *utrd = dma.virt;
	uint8_t *ucd = dma.virt + ucdOffset;
	zeroWords(utrd, 32);
	zeroWords(ucd, 1024 + 16);
	ucd[0] = 0x00;   // NOP OUT
	ucd[3] = 1;      // task tag
	uint64_t ucdPhys = list + ucdOffset;
	*reinterpret_cast<uint32_t *>(utrd + 0) = 0x10000000U;
	*reinterpret_cast<uint32_t *>(utrd + 8) = 0x0fU;
	*reinterpret_cast<uint32_t *>(utrd + 16) = (uint32_t)ucdPhys;
	*reinterpret_cast<uint32_t *>(utrd + 20) = (uint32_t)(ucdPhys >> 32);
	*reinterpret_cast<uint32_t *>(utrd + 24) = (512U >> 2) | ((kMinRsp >> 2) << 16);
	*reinterpret_cast<uint32_t *>(utrd + 28) = (kMinPrdt >> 2) << 16;
	cacheClean(dma.virt, 16384);
	writeReg(hc, SM_UFS_REG_IS, 0xffffffffU);
	fbMark(3);
	writeReg(hc, SM_UFS_REG_UTRLDBR, 1);
	bool done = false;
	for (uint32_t loops = 0; loops < 100000; loops++) {
		if ((readReg(hc, SM_UFS_REG_UTRLDBR) & 1) == 0) {
			done = true;
			gSmUfs.minPolls = loops;
			break;
		}
		IODelay(1);
	}
	fbMark(4);
	fbMark2(4);
	cacheCleanInvalidate(dma.virt, 16384);
	gSmUfs.minIsAfter = readReg(hc, SM_UFS_REG_IS);
	gSmUfs.minDbrAfter = readReg(hc, SM_UFS_REG_UTRLDBR);
	gSmUfs.minRsrAfter = readReg(hc, SM_UFS_REG_UTRLRSR);
	gSmUfs.minOcs = *reinterpret_cast<volatile uint32_t *>(utrd + 8) & 0xff;
	const uint8_t *rsp = ucd + kMinRsp;
	gSmUfs.minRsp = rsp[6];
	gSmUfs.minScsi = rsp[7];
	gSmUfs.minStep = 5;
	return done ? 0 : 0x80;
}

namespace {

// Level 15 is the kebab driver's (OnePlus 8T, same SM8250, reaches userdata) takeover. Every
// earlier path reused the firmware's request list; this one points the host at memory the driver
// owns, proves the host took it (UTRLBA readback), and only then rings. Nothing else is touched:
// no HCE cycle, no link startup, no clock or ICE writes. The bootloader left all of that running
// and read this kernel through the same host.
struct SmUfsOwnDma {
	IOBufferMemoryDescriptor *descriptor;
	uint8_t *virt;
	uint64_t phys;
};

enum : uint32_t {
	kKbList = 0x0000,        // 32 UTRDs, 1 KiB
	kKbUcd = 0x1000,         // command UPIU, response UPIU, PRDT
	kKbBounce = 0x4000,
	kKbBounceSize = 0x10000,
	kKbSize = kKbBounce + kKbBounceSize,
};

bool allocOwnDma(SmUfsOwnDma &dma)
{
	IOOptionBits options = (IOOptionBits)kIODirectionInOut |
	                       (IOOptionBits)kIOMemoryPhysicallyContiguous |
	                       (IOOptionBits)kIOMemoryMapperNone;
	// Level 16 forces the request list below 4 GiB: a descriptor fetch that silently truncated the
	// high address word would wedge exactly where the first doorbell does. Fall back to an
	// unrestricted allocation when low memory cannot satisfy the request.
	if (gSmUfs.level >= 16 && gSmUfs.level != 20) {
		dma.descriptor = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, options,
		                                                                 kKbSize, 0xffffffffULL);
		if (dma.descriptor != nullptr) {
			gSmUfs.kbDmaLow = 1;
		}
	}
	if (dma.descriptor == nullptr) {
		dma.descriptor = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, options, kKbSize,
		                                                            0x4000);
	}
	if (dma.descriptor == nullptr) {
		return false;
	}
	dma.virt = static_cast<uint8_t *>(dma.descriptor->getBytesNoCopy());
	IOByteCount contiguous = 0;
	dma.phys = dma.descriptor->getPhysicalSegment(0, &contiguous, kIOMemoryMapperNone);
	gSmUfs.kbDmaContig = (uint32_t)contiguous;
	if (dma.virt == nullptr || dma.phys == 0 || (dma.phys & 0xfff) != 0 ||
	    contiguous < kKbSize) {
		return false;
	}
	zeroWords(dma.virt, kKbSize);
	cacheCleanInvalidate(dma.virt, kKbSize);
	return true;
}

// Level 17: the doorbell store runs on a helper thread. A store that never retires then only
// wedges that thread; the probe thread reports from memory within 50 ms, long before the stalled
// transaction takes the display and the panic screen with it. The helper also polls and records
// the completion, so the probe thread never reads the UFS registers after the ring.
volatile uint32_t gKbRingState;   // 0 idle, 1 store in progress, 2 store retired, 4 done, 5 timeout
uint32_t gKbRingSlot;             // UTRLDBR bit; 0 for every level that owns slot 0
uintptr_t gKbRingHc;
uint8_t *gKbRingVirt;
uint8_t *gKbRingUtrd;
uint8_t *gKbRingUcd;

void kbRingThread(void *, wait_result_t)
{
	__atomic_store_n(&gKbRingState, 1, __ATOMIC_RELEASE);
	writeReg(gKbRingHc, SM_UFS_REG_UTRLDBR, 1u << gKbRingSlot);
	__atomic_store_n(&gKbRingState, 2, __ATOMIC_RELEASE);
	bool completed = false;
	for (uint32_t loops = 0; loops < 250; loops++) {
		uint32_t dbr = readReg(gKbRingHc, SM_UFS_REG_UTRLDBR);
		gSmUfs.kbRingDbr = dbr;
		if ((dbr & (1u << gKbRingSlot)) == 0) {
			completed = true;
			break;
		}
		uint32_t is = readReg(gKbRingHc, SM_UFS_REG_IS);
		gSmUfs.kbRingIs = is;
		if (is & 0x70800U) {
			break;
		}
		IODelay(1000);
	}
	uint32_t is = readReg(gKbRingHc, SM_UFS_REG_IS);
	gSmUfs.kbRingIs = is;
	writeReg(gKbRingHc, SM_UFS_REG_IS, is);
	gSmUfs.kbRingDbr = readReg(gKbRingHc, SM_UFS_REG_UTRLDBR);
	cacheInvalidate(gKbRingVirt, kKbSize);
	gSmUfs.kbRingPoll = 1;
	if (completed) {
		const uint8_t *rsp = gKbRingUcd + 512;
		gSmUfs.kbRingOcs = *reinterpret_cast<volatile uint32_t *>(gKbRingUtrd + 8) & 0xf;
		gSmUfs.kbRingRsp = rsp[6];
		gSmUfs.kbRingScsi = rsp[7];
		gSmUfs.kbRingSense = ((uint32_t)(rsp[36] & 0x0f) << 8) | rsp[44];
		__atomic_store_n(&gKbRingState, 4, __ATOMIC_RELEASE);
	} else {
		__atomic_store_n(&gKbRingState, 5, __ATOMIC_RELEASE);
	}
}

SmUfsCommandResult ownSubmitThreaded(uintptr_t hc, uint8_t *utrd, uint8_t *ucd, uint8_t *virt,
                                     uint32_t slot = 0)
{
	SmUfsCommandResult result = {0xf, 0xff, 0xff, 0};
	gKbRingSlot = slot;
	gKbRingHc = hc;
	gKbRingVirt = virt;
	gKbRingUtrd = utrd;
	gKbRingUcd = ucd;
	gSmUfs.kbRingDbr = 0;
	gSmUfs.kbRingIs = 0;
	gSmUfs.kbRingStore = 0;
	gSmUfs.kbRingPoll = 0;
	__atomic_store_n(&gKbRingState, 0, __ATOMIC_RELEASE);
	thread_t thread = nullptr;
	bool started = kernel_thread_start(kbRingThread, nullptr, &thread) == KERN_SUCCESS;
	if (started) {
		thread_deallocate(thread);
	}
	bool entered = false;
	for (unsigned i = 0; i < 100; i++) {
		if (__atomic_load_n(&gKbRingState, __ATOMIC_ACQUIRE) != 0) {
			entered = true;
			break;
		}
		IODelay(1000);
	}
	if (!started || !entered) {
		gSmUfs.kbPhase = 10;
		ringReportPanic();
	}
	gSmUfs.kbPhase = 11;
	bool stored = false;
	for (unsigned i = 0; i < 50; i++) {
		if (__atomic_load_n(&gKbRingState, __ATOMIC_ACQUIRE) >= 2) {
			stored = true;
			break;
		}
		IODelay(1000);
	}
	if (!stored) {
		ringReportPanic();
	}
	gSmUfs.kbRingStore = 1;
	gSmUfs.kbPhase = 12;
	fbMark2(2);
	gSmUfs.kbFb2 = 2;
	bool finished = false;
	for (unsigned i = 0; i < 300; i++) {
		if (__atomic_load_n(&gKbRingState, __ATOMIC_ACQUIRE) >= 4) {
			finished = true;
			break;
		}
		IODelay(1000);
	}
	if (!finished) {
		gSmUfs.kbPhase = 13;
		ringReportPanic();
	}
	result.ocs = gSmUfs.kbRingOcs;
	result.response = gSmUfs.kbRingRsp;
	result.scsi = gSmUfs.kbRingScsi;
	result.sense = gSmUfs.kbRingSense;
	return result;
}

// Level 22: ring slot 0 with no valid UTRD (Own=0). If the doorbell still never retires, the
// UTP slave itself is not running and no descriptor fetch is involved. If it retires, the marker
// sequence distinguishes "engine processed the empty slot" (row 3, DBR cleared) from "store
// returned but DBR stayed" (row 4); either way the E2 question is answered without a panic.
void ownRingEmpty(uintptr_t hc, const SmUfsOwnDma &dma)
{
	uint8_t *utrd = dma.virt + kKbList;
	uint8_t *ucd = dma.virt + kKbUcd;
	zeroWords(utrd, 32);
	zeroWords(ucd, 1040);
	gSmUfs.kbPhase = 10;
	cacheCleanInvalidate(dma.virt, kKbSize);
	fbMark2(1);
	gSmUfs.kbFb2 = 1;
	gKbRingHc = hc;
	gKbRingVirt = dma.virt;
	gKbRingUtrd = utrd;
	gKbRingUcd = ucd;
	gSmUfs.kbRingDbr = 0;
	gSmUfs.kbRingIs = 0;
	gSmUfs.kbRingStore = 0;
	gSmUfs.kbRingPoll = 0;
	__atomic_store_n(&gKbRingState, 0, __ATOMIC_RELEASE);
	thread_t thread = nullptr;
	bool started = kernel_thread_start(kbRingThread, nullptr, &thread) == KERN_SUCCESS;
	if (started) {
		thread_deallocate(thread);
	}
	bool entered = false;
	for (unsigned i = 0; i < 100; i++) {
		if (__atomic_load_n(&gKbRingState, __ATOMIC_ACQUIRE) != 0) {
			entered = true;
			break;
		}
		IODelay(1000);
	}
	if (!started || !entered) {
		gSmUfs.kbPhase = 10;
		ringReportPanic();
	}
	gSmUfs.kbPhase = 11;
	bool stored = false;
	for (unsigned i = 0; i < 50; i++) {
		if (__atomic_load_n(&gKbRingState, __ATOMIC_ACQUIRE) >= 2) {
			stored = true;
			break;
		}
		IODelay(1000);
	}
	if (!stored) {
		ringReportPanic();
	}
	gSmUfs.kbRingStore = 1;
	gSmUfs.kbPhase = 12;
	fbMark2(2);
	gSmUfs.kbFb2 = 2;
	bool finished = false;
	for (unsigned i = 0; i < 1200; i++) {
		if (__atomic_load_n(&gKbRingState, __ATOMIC_ACQUIRE) >= 4) {
			finished = true;
			break;
		}
		IODelay(1000);
	}
	gSmUfs.kbPhase = 13;
	if (finished && (gSmUfs.kbRingDbr & 1) == 0) {
		fbMark2(3);
		gSmUfs.kbFb2 = 3;
	} else {
		fbMark2(4);
		gSmUfs.kbFb2 = 4;
	}
	ringReportPanic();
}

// Send one UIC command and record the result. Used for DME_HIBER_EXIT: the bootloader's last UIC
// command completed (pre IS is UIC_COMMAND_COMPL) and the device may be parked in H8, where the
// doorbell never retires. The UIC path itself is known to work.
uint32_t sendUicOpcode(uintptr_t hc, uint32_t opcode, uint32_t &hcsOut, uint32_t &isOut)
{
	writeReg(hc, SM_UFS_REG_IS, SM_UIC_COMMAND_COMPL);
	writeReg(hc, SM_UFS_REG_UICARG1, 0);
	writeReg(hc, SM_UFS_REG_UICARG2, 0);
	writeReg(hc, SM_UFS_REG_UICARG3, 0);
	writeReg(hc, SM_UFS_REG_UICCMD, opcode);
	bool done = false;
	for (unsigned loop = 0; loop < 10000; loop++) {
		if (readReg(hc, SM_UFS_REG_IS) & SM_UIC_COMMAND_COMPL) {
			done = true;
			break;
		}
		IODelay(10);
	}
	uint32_t result = done ? (readReg(hc, SM_UFS_REG_UICARG2) & 0xff) : 0x55;
	isOut = readReg(hc, SM_UFS_REG_IS);
	writeReg(hc, SM_UFS_REG_IS, SM_UIC_COMMAND_COMPL);
	hcsOut = readReg(hc, SM_UFS_REG_HCS);
	return result;
}

// One polled SCSI command in slot 0, built the way the kebab driver builds it: UTRD interrupt bit
// set, OCS invalid before the ring, a 512-byte response UPIU, and the PRDT 1024 bytes into the UCD.
SmUfsCommandResult ownSubmit(uintptr_t hc, const SmUfsOwnDma &dma, const uint8_t *cdb,
                             unsigned cdbLength, bool toHost, uint32_t length)
{
	SmUfsCommandResult result = {0xf, 0xff, 0xff, 0};
	uint8_t *utrd = dma.virt + kKbList;
	uint8_t *ucd = dma.virt + kKbUcd;
	zeroWords(utrd, 32);
	zeroWords(ucd, 1040);
	ucd[0] = 0x01;                                    // COMMAND UPIU
	ucd[1] = length ? (toHost ? 0x40 : 0x20) : 0x00;  // device-to-memory / memory-to-device
	ucd[2] = 0;
	putBe32(ucd + 12, length);
	for (unsigned index = 0; index < cdbLength; index++) {
		ucd[16 + index] = cdb[index];
	}

	uint64_t ucdPhys = dma.phys + kKbUcd;
	uint64_t bouncePhys = dma.phys + kKbBounce;
	*reinterpret_cast<uint32_t *>(utrd + 0) =
	    0x11000000U | ((length ? (toHost ? 2U : 1U) : 0U) << 25);
	*reinterpret_cast<uint32_t *>(utrd + 8) = 0x0fU;  // OCS invalid until the host updates it
	*reinterpret_cast<uint32_t *>(utrd + 16) = (uint32_t)ucdPhys;
	*reinterpret_cast<uint32_t *>(utrd + 20) = (uint32_t)(ucdPhys >> 32);
	*reinterpret_cast<uint32_t *>(utrd + 24) = (512U >> 2) | ((512U >> 2) << 16);
	*reinterpret_cast<uint32_t *>(utrd + 28) = (1024U >> 2) << 16;
	if (length) {
		uint8_t *prd = ucd + 1024;
		*reinterpret_cast<uint32_t *>(prd + 0) = (uint32_t)bouncePhys;
		*reinterpret_cast<uint32_t *>(prd + 4) = (uint32_t)(bouncePhys >> 32);
		*reinterpret_cast<uint32_t *>(prd + 12) = length - 1;
		*reinterpret_cast<uint32_t *>(utrd + 28) = 1U | ((1024U >> 2) << 16);
	}
	gSmUfs.kbPhase = 10;
	cacheCleanInvalidate(dma.virt, kKbSize);
	fbMark2(1);
	gSmUfs.kbFb2 = 1;
	if (gSmUfs.level == 23) {
		fbHibMark();
	}
	if (gSmUfs.level >= 17) {
		return ownSubmitThreaded(hc, utrd, ucd, dma.virt);
	}

	// The marker between the store and the first poll is the whole point: if the board dies with
	// the second row at 2, the doorbell store retired and the stall is in the fetch/reads after it.
	gSmUfs.kbPhase = 11;
	writeReg(hc, SM_UFS_REG_UTRLDBR, 1);
	gSmUfs.kbPhase = 12;
	fbMark2(2);
	gSmUfs.kbFb2 = 2;
	bool completed = false;
	// 250 ms per command: the take-over issues at most five and the watchdog fires 1.5 s after the
	// probe starts. A command still outstanding then is not going to finish.
	for (uint32_t loops = 0; loops < 250; loops++) {
		uint32_t dbr = readReg(hc, SM_UFS_REG_UTRLDBR);
		if (loops == 0) {
			gSmUfs.kbPhase = 13;
			fbMark2(3);
			gSmUfs.kbFb2 = 3;
		}
		if ((dbr & 1) == 0) {
			completed = true;
			break;
		}
		if (readReg(hc, SM_UFS_REG_IS) & 0x70800U) {
			break;
		}
		IODelay(1000);
	}
	gSmUfs.kbPhase = 14;
	if (completed) {
		fbMark2(4);
		gSmUfs.kbFb2 = 4;
	}
	gSmUfs.kbIsAfter = readReg(hc, SM_UFS_REG_IS);
	gSmUfs.kbPhase = 15;
	writeReg(hc, SM_UFS_REG_IS, gSmUfs.kbIsAfter);   // write-one-to-clear after saving it
	gSmUfs.kbPhase = 16;
	gSmUfs.kbDbrAfter = readReg(hc, SM_UFS_REG_UTRLDBR);
	cacheInvalidate(dma.virt, kKbSize);
	gSmUfs.kbPhase = 17;
	if (!completed) {
		return result;
	}
	result.ocs = *reinterpret_cast<volatile uint32_t *>(utrd + 8) & 0xf;
	const uint8_t *rsp = ucd + 512;
	result.response = rsp[6];
	result.scsi = rsp[7];
	// Response UPIU sense data: [32:33] length, [34] response code, [36] sense key, [44] ASC.
	result.sense = ((uint32_t)(rsp[36] & 0x0f) << 8) | rsp[44];
	return result;
}

}  // namespace

void ringReportPanic(void)
{
	panic("SMUFS ring17 phase=%x step=%x fb2 %x st %x\n"
	      "dma %llx low%x cap %08x\n"
	      "clk %08x>%08x wild r%x s%x b%x\n"
	      "smr %08x s2cr %x\n"
	      "dbr %08x is %08x store %x poll %x\n"
	      "cold %x link %x hcs %x is %x\n"
	      "hib %x hcs %x is %x\n"
      "tur %x/%02x/%02x s%03x n%u\n",
	      gSmUfs.kbPhase, gSmUfs.kbStep, gSmUfs.kbFb2, (uint32_t)gKbRingState,
	      gSmUfs.kbDmaPa, gSmUfs.kbDmaLow, gSmUfs.kbCapReg,
	      gSmUfs.kbClkReqBefore, gSmUfs.kbClkReqAfter,
	      gSmUfs.wildResult, gSmUfs.wildSlot, gSmUfs.wildBank,
	      gSmUfs.wildSmr, gSmUfs.wildS2cr,
	      gSmUfs.kbRingDbr, gSmUfs.kbRingIs, gSmUfs.kbRingStore, gSmUfs.kbRingPoll,
	      gSmUfs.kbColdResult, gSmUfs.kbLinkResult, gSmUfs.kbLinkHcs, gSmUfs.kbLinkIs,
	      gSmUfs.kbHiberResult, gSmUfs.kbHiberHcs, gSmUfs.kbHiberIs,
	      gSmUfs.kbTurOcs, gSmUfs.kbTurRsp, gSmUfs.kbTurScsi, gSmUfs.kbTurSense,
	      gSmUfs.kbTurTries);
}

// Level 26: adopt the bootloader's request list instead of replacing it. The vendor UEFI driver
// in this same firmware never writes UTRLBA or UTRLRSR when it takes the controller over, and
// ShengUfs upstream documents the same shape ("adopts an existing firmware request list and uses
// a polled request slot"). Every earlier level rewrites the base and toggles the run-stop bit, and
// the first fetch after that rewrite never retires the doorbell store. Here the list base and the
// run-stop bit stay exactly as the bootloader left them: take a slot whose Own bit is clear, fill
// it with our own UCD/PRDT, clean the caches and ring that slot only.
int NativeUfs::adoptPure(SmUfsMaps &map)
{
	uintptr_t hc = map.hc;
	gSmUfs.kbHiberResult = 0xee;
	gSmUfs.kbHce = readReg(hc, SM_UFS_REG_HCE);
	gSmUfs.kbHcs = readReg(hc, SM_UFS_REG_HCS);
	gSmUfs.kbDbr = readReg(hc, SM_UFS_REG_UTRLDBR);
	gSmUfs.kbTmrl = readReg(hc, 0x78);
	gSmUfs.kbIs = readReg(hc, SM_UFS_REG_IS);
	gSmUfs.kbRsr = readReg(hc, SM_UFS_REG_UTRLRSR);
	gSmUfs.kbCapReg = readReg(hc, SM_UFS_REG_CAP);
	// The bootloader read this kernel through the host, so the list must still be started, the
	// link up and nothing may be outstanding. Anything else means this is not an adoptable state.
	if ((gSmUfs.kbHce & 1) == 0 || (gSmUfs.kbHcs & 3) != 3 || (gSmUfs.kbRsr & 1) == 0 ||
	    gSmUfs.kbDbr != 0 || gSmUfs.kbTmrl != 0) {
		report15();
	}
	fbMark(1);

	uint64_t listPa = ((uint64_t)readReg(hc, SM_UFS_REG_UTRLBAU) << 32) |
	                  (uint64_t)readReg(hc, SM_UFS_REG_UTRLBA);
	gSmUfs.kbUtrlba = listPa;
	// UTRLBA must be 512-byte aligned per UFSHCI; anything else is not a request list.
	if (listPa == 0 || (listPa & 0x1ff) != 0) {
		report15();
	}
	fbMark(2);

	SmUfsOwnDma dma = {};
	if (!allocOwnDma(dma)) {
		report15();
	}
	gSmUfs.kbDmaPa = dma.phys;
	gSmUfs.kbStep = 1;

	OSSharedPtr<IOMemoryMap> listMap;
	uintptr_t list = mapPhys(listPa, 0x1000, listMap);
	if (list == 0) {
		report15();
	}
	// The bootloader wrote these UTRDs through a cacheable mapping and the controller clears
	// Own in DRAM; without the invalidate the stale lines read every slot as busy.
	cacheInvalidate((void *)list, 0x1000);
	// CAP[20:16] is NUTRS, the number of request slots minus one. Clamp to the architectural 32.
	uint32_t slots = ((gSmUfs.kbCapReg >> 16) & 0x1f) + 1;
	if (slots > 32) {
		slots = 32;
	}
	uint32_t slot = 0xffffffffU;
	gSmUfs.kbPureDw0 = *(volatile uint32_t *)(list);
	for (uint32_t i = 0; i < slots; i++) {
		if ((*(volatile uint32_t *)(list + i * 32) & 1U) == 0) {
			slot = i;
			break;
		}
	}
	gSmUfs.kbPureSlot = slot;
	if (slot == 0xffffffffU) {
		report15();
	}
	gSmUfs.kbStep = 2;
	fbMark(3);

	uint8_t *utrd = (uint8_t *)(list + slot * 32);
	uint8_t *ucd = dma.virt + kKbUcd;
	zeroWords(utrd, 32);
	zeroWords(ucd, 1040);
	ucd[0] = 0x01;                                    // COMMAND UPIU
	ucd[1] = 0x00;
	uint64_t ucdPhys = dma.phys + kKbUcd;
	*reinterpret_cast<uint32_t *>(utrd + 0) = 0x11000000U;
	*reinterpret_cast<uint32_t *>(utrd + 8) = 0x0fU;  // OCS invalid until the host updates it
	*reinterpret_cast<uint32_t *>(utrd + 16) = (uint32_t)ucdPhys;
	*reinterpret_cast<uint32_t *>(utrd + 20) = (uint32_t)(ucdPhys >> 32);
	*reinterpret_cast<uint32_t *>(utrd + 24) = (512U >> 2) | ((512U >> 2) << 16);
	*reinterpret_cast<uint32_t *>(utrd + 28) = (1024U >> 2) << 16;
	gSmUfs.kbPhase = 10;
	// The host is not cache coherent: the UTRD in the borrowed list and the UCD/PRDT in our page
	// must reach memory before the doorbell.
	cacheCleanInvalidate(dma.virt, kKbSize);
	cacheCleanInvalidate(utrd, 32);
	gSmUfs.kbStep = 3;

	SmUfsCommandResult t = ownSubmitThreaded(hc, utrd, ucd, dma.virt, slot);
	gSmUfs.kbTurOcs = t.ocs;
	gSmUfs.kbTurRsp = t.response;
	gSmUfs.kbTurScsi = t.scsi;
	gSmUfs.kbTurSense = t.sense;
	if (t.ocs != 0x0fu) {
		fbMark2(4);
		gSmUfs.kbFb2 = 4;
	}
	return 0;
}

// Level 34: put the two UFS SMMU streams into BYPASS so the controller's fetch of our own
// descriptors uses physical addresses. GFander's SmmuBypassDxe documents the IDs (ARID 0x0 ->
// SID 0x4E0, ARID 0x10 -> SID 0xE0) and refuses wildcard entries so no shared stream is changed.
static uint32_t smmuBypassUfsStreams(void)
{
	OSSharedPtr<IOMemoryMap> smmuMap;
	uintptr_t s = mapPhys(0x15000000ULL, 0xd0000, smmuMap);
	if (s == 0) {
		return 0xffffffffU;
	}
	uint32_t changed = 0;
	for (uint32_t i = 0; i < 128; i++) {
		uint32_t smr = readReg(s, 0x800 + i * 4);
		if ((smr & 0x80000000U) == 0) {
			continue;
		}
		uint32_t sid = smr & 0xffffU;
		uint32_t mask = (smr >> 16) & 0xffffU;
		if (mask != 0) {
			continue;                       // never touch a wildcard shared with other devices
		}
		if (sid != 0x04e0U && sid != 0x00e0U) {
			continue;
		}
		uint32_t s2cr = readReg(s, 0xc00 + i * 4);
		s2cr &= ~(3U << 16);
		s2cr |= (1U << 16);                     // TYPE = BYPASS
		writeReg(s, 0xc00 + i * 4, s2cr);
		changed++;
	}
	return changed;
}

// Map the UFS host stream through a bypass context bank the way ShengUfs does on
// alioth (same SM8250). Returns 1 on success, 0xffffffff if the SMMU cannot be
// mapped, 0x1xx on a plan error and 0x2xx on a configure/readback error.
static uint32_t smmuAllowStream(uint16_t stream, uint64_t dma, uint64_t bytes)
{
	OSSharedPtr<IOMemoryMap> keep;
	uintptr_t s = mapPhys(0x15000000ULL, 0xd0000, keep);
	if (s == 0) {
		return 0xffffffffU;
	}
	auto rd = [s](unsigned off) { return readReg(s, off); };
	auto wr = [s](unsigned off, uint32_t v) {
		writeReg(s, off, v);
		__asm__ volatile("dsb sy" ::: "memory");
	};
	UfsSmmuPlan plan = planUfsSmmu(stream, dma, bytes, rd);
	if (plan.error) {
		// Named check first: the geometry checks differ between SMMU revisions, and the
		// IDRs decide which one refused. (level 38 diagnostic)
		panic("SMUFS smmu plan: %s idx=%x idr0 %08x idr1 %08x idr2 %08x",
		      plan.error, (uint32_t)plan.slot, readReg(s, 0x20), readReg(s, 0x24), readReg(s, 0x28));
	}
	const char *failure = configureUfsSmmu(plan, stream, rd, wr);
	if (failure) {
		// The planner takes the highest free stream-match group, which the firmware may
		// reserve. Try the low groups too before giving up (level 38 diagnostic).
		for (unsigned retry = 0; retry < 4; retry++) {
			UfsSmmuPlan alt = plan;
			alt.slot = retry;
			const char *second = configureUfsSmmu(alt, stream, rd, wr);
			if (!second) {
				if (gSmUfs.level == 71)
					printf("PLATFORM-DMA ufs-route sid=%x bank=%u slot=%u\n", stream, alt.bank, alt.slot);
				return 1;
			}
		}
		panic("SMUFS smmu configure: %s slot=%x bank=%u smr=%08x s2cr=%08x cbar=%08x cb=%08x",
		      failure, (uint32_t)plan.slot, plan.bank,
		      readReg(s, 0x800 + 4 * plan.slot), readReg(s, 0xc00 + 4 * plan.slot),
		      readReg(s, 0x1000 + 4 * plan.bank),
		      readReg(s, 0x1000 * plan.pages + 0x1000 * plan.bank));
	}
	if (gSmUfs.level == 71)
		printf("PLATFORM-DMA ufs-route sid=%x bank=%u slot=%u\n", stream, plan.bank, plan.slot);
	return 1;
}

int NativeUfs::adoptKebab(SmUfsMaps &map)
{
	uintptr_t hc = map.hc;
		if (gSmUfs.level == 28 || gSmUfs.level == 36) {
			// The vendor driver powers the PHY before it stops the controller: UfsHwInit calls
			// its PHY init (0x1683c) before the HCE=0 store. Level 27 skipped that and the UFS
			// register block read as zero afterwards (CAP=0, ICE=0, HCE would not set), so the
			// domain and the nine branch clocks are brought up first, exactly as coldStart does.
			if (powerGdsc(map.gcc) != 0) {
				report15();
			}
			uint32_t clockFailed = 0;
			clockFailed |= (uint32_t)enableBranch(map.gcc, SM_GCC_UFS_AXI_CBCR) << 0;
			clockFailed |= (uint32_t)enableBranch(map.gcc, SM_GCC_UFS_AHB_CBCR) << 1;
			clockFailed |= (uint32_t)enableBranch(map.gcc, SM_GCC_UFS_UNIPRO_CORE_CBCR) << 2;
			clockFailed |= (uint32_t)enableBranch(map.gcc, SM_GCC_AGGRE_UFS_AXI_CBCR) << 3;
			clockFailed |= (uint32_t)enableBranch(map.gcc, SM_GCC_UFS_PHY_AUX_CBCR) << 4;
			clockFailed |= (uint32_t)enableBranch(map.gcc, SM_GCC_UFS_ICE_CORE_CBCR) << 5;
			clockFailed |= (uint32_t)enableBranch(map.gcc, SM_GCC_UFS_TX_SYMBOL0_CBCR) << 6;
			clockFailed |= (uint32_t)enableBranch(map.gcc, SM_GCC_UFS_RX_SYMBOL0_CBCR) << 7;
			clockFailed |= (uint32_t)enableBranch(map.gcc, SM_GCC_UFS_RX_SYMBOL1_CBCR) << 8;
			if (clockFailed != 0) {
				report15();
			}
		}
	if (gSmUfs.level == 33) {
		// The vendor's light cycle: stop the controller, reset the PHY through GCC BCR only,
		// bring it back and take the list over - no coldStart tables, no DME_LINK_STARTUP.
		if (powerGdsc(map.gcc) != 0) {
			report15();
		}
		uint32_t clockFailed = 0;
		clockFailed |= (uint32_t)enableBranch(map.gcc, SM_GCC_UFS_AXI_CBCR) << 0;
		clockFailed |= (uint32_t)enableBranch(map.gcc, SM_GCC_UFS_AHB_CBCR) << 1;
		clockFailed |= (uint32_t)enableBranch(map.gcc, SM_GCC_UFS_UNIPRO_CORE_CBCR) << 2;
		clockFailed |= (uint32_t)enableBranch(map.gcc, SM_GCC_AGGRE_UFS_AXI_CBCR) << 3;
		clockFailed |= (uint32_t)enableBranch(map.gcc, SM_GCC_UFS_PHY_AUX_CBCR) << 4;
		clockFailed |= (uint32_t)enableBranch(map.gcc, SM_GCC_UFS_ICE_CORE_CBCR) << 5;
		clockFailed |= (uint32_t)enableBranch(map.gcc, SM_GCC_UFS_TX_SYMBOL0_CBCR) << 6;
		clockFailed |= (uint32_t)enableBranch(map.gcc, SM_GCC_UFS_RX_SYMBOL0_CBCR) << 7;
		clockFailed |= (uint32_t)enableBranch(map.gcc, SM_GCC_UFS_RX_SYMBOL1_CBCR) << 8;
		if (clockFailed != 0) {
			report15();
		}
		writeReg(hc, SM_UFS_REG_HCE, 0);
		for (unsigned loop = 0; loop < 1000; loop++) {
			if ((readReg(hc, SM_UFS_REG_HCE) & 1) == 0) {
				break;
			}
			IODelay(10);
		}
		writeReg(map.gcc, SM_GCC_UFS_BCR, 1);
		IODelay(200);
		writeReg(map.gcc, SM_GCC_UFS_BCR, 0);
		IODelay(200);
		writeReg(hc, SM_UFS_REG_HCE, 1);
		for (unsigned loop = 0; loop < 1000; loop++) {
			if (readReg(hc, SM_UFS_REG_HCE) & 1) {
				break;
			}
			IODelay(10);
		}
	}
	if (gSmUfs.level == 35 || gSmUfs.level == 36) {
		// The vendor opens the ICE/IOC side while the controller is still running: 0x1D98004
		// = 0x5F, a 1 ms wait, and only then HCE=0. Every earlier level poked after the stop,
		// when the register block already read as zero.
		OSSharedPtr<IOMemoryMap> icePreMap;
		uintptr_t icePre = mapPhys(0x1d98000ULL, 0x1000, icePreMap);
		if (icePre != 0) {
			writeReg(icePre, 0x4, 0x5f);
			gSmUfs.kbPokeIce = readReg(icePre, 0x4);
		}
		IODelay(1000);
	}
	if (gSmUfs.level == 27 || gSmUfs.level == 28 || gSmUfs.level == 29 ||
	    gSmUfs.level == 35 || gSmUfs.level == 36) {
		// The vendor UEFI driver's own cold sequence (UfsHwInit 0x11a24): controller off,
		// CFG1 written literally with the PHY soft-reset bit still set, PCS released, the
		// ICE-side IOC poke, then the controller back on. It never runs our QMP/SerDes/PCS
		// tables, so this tests whether those three steps are what makes its doorbell work.
		writeReg(hc, SM_UFS_REG_HCE, 0);
		for (unsigned loop = 0; loop < 1000; loop++) {
			if ((readReg(hc, SM_UFS_REG_HCE) & 1) == 0) {
				break;
			}
			IODelay(10);
		}
		// Level 29 keeps CFG1 bit 26 (set in the XBL value 0x1C00052C): clearing it made the
		// whole register block read as zero and HCE never set again, while level 25 (an HCE
		// cycle with a CFG1 read-modify-write) enables fine. The vendor low bits are kept,
		// including the PHY soft-reset bit it holds.
		uint32_t vendorCfg1 = (gSmUfs.level == 29) ? 0x1c00052eU : 0x1800052eU;
		writeReg(hc, SM_UFS_REG_CFG1, vendorCfg1);
		gSmUfs.kbPokeCfg1 = readReg(hc, SM_UFS_REG_CFG1);
		OSSharedPtr<IOMemoryMap> icePokeMap;
		uintptr_t icePoke = mapPhys(0x1d98000ULL, 0x1000, icePokeMap);
		writeReg(map.phy, 0xc00, 1);              // 0x1D87C00: release the PCS
		for (unsigned loop = 0; loop < 200; loop++) {
			if ((readReg(map.phy, 0xbc) & 0xffffU) == 0x3dU) {
				break;
			}
			IODelay(10);
		}
		gSmUfs.kbPokePcs = readReg(map.phy, 0xbc);   // 0x1D870BC
		if (icePoke != 0) {
			writeReg(icePoke, 0x4, 0x5f);             // 0x1D98004: vendor IOC poke
			gSmUfs.kbPokeIce = readReg(icePoke, 0x4);
		}
		writeReg(hc, SM_UFS_REG_HCE, 1);
		for (unsigned loop = 0; loop < 1000; loop++) {
			if (readReg(hc, SM_UFS_REG_HCE) & 1) {
				break;
			}
			IODelay(10);
		}
	}
	gSmUfs.kbHiberResult = 0xee;   // not sent unless level 23 reaches sendUicOpcode
	gSmUfs.kbHce = readReg(hc, SM_UFS_REG_HCE);
	gSmUfs.kbHcs = readReg(hc, SM_UFS_REG_HCS);
	gSmUfs.kbDbr = readReg(hc, SM_UFS_REG_UTRLDBR);
	gSmUfs.kbTmrl = readReg(hc, 0x78);
	gSmUfs.kbIs = readReg(hc, SM_UFS_REG_IS);
	// The bootloader read this kernel through the host, so it must be enabled, link-up and have a
	// started transfer list with nothing outstanding. Anything else means it is not ours to take.
	if ((gSmUfs.kbHce & 1) == 0 || (gSmUfs.kbHcs & 1) == 0 || (gSmUfs.kbHcs & 2) == 0) {
		report15();
	}
	if (gSmUfs.kbDbr != 0 || gSmUfs.kbTmrl != 0 || (gSmUfs.kbIs & 0x70800U) != 0) {
		report15();
	}
	fbMark(1);
	SmUfsOwnDma dma = {};
	if (!allocOwnDma(dma)) {
		report15();
	}
	gSmUfs.kbDmaPa = dma.phys;
	gSmUfs.kbStep = 1;
	fbMark(2);

	if (gSmUfs.level == 25 || gSmUfs.level == 30 || gSmUfs.level == 31 || gSmUfs.level == 32 ||
	    gSmUfs.level == 34) {
		writeReg(hc, SM_UFS_REG_HCE, 0);
		for (unsigned loop = 0; loop < 1000; loop++) {
			if ((readReg(hc, SM_UFS_REG_HCE) & 1) == 0) {
				break;
			}
			IODelay(10);
		}
		gSmUfs.kbColdResult = (uint32_t)coldStart(map);
		if (gSmUfs.level == 30 || gSmUfs.level == 31 || gSmUfs.level == 32 ||
		    gSmUfs.level == 34 || gSmUfs.level == 35 || gSmUfs.level == 36) {
			// The vendor pokes, applied on the fully cold-started controller: its CFG1 low bits
			// (bit 26 kept, since clearing it kills the register block here), the PCS release
			// and the ICE-side IOC write that EnableUfsIOC gates.
			// Level 31 skips this rewrite: bit 1 is the PHY soft reset, and re-asserting
			// it after coldStart left DME_LINK_STARTUP at 0x55 in level 30.
			if (gSmUfs.level == 30) {
				writeReg(hc, SM_UFS_REG_CFG1, 0x1c00052eU);
				gSmUfs.kbPokeCfg1 = readReg(hc, SM_UFS_REG_CFG1);
			}
			OSSharedPtr<IOMemoryMap> icePokeMap;
			uintptr_t icePoke = mapPhys(0x1d98000ULL, 0x1000, icePokeMap);
			writeReg(map.phy, 0xc00, 1);
			for (unsigned loop = 0; loop < 200; loop++) {
				if ((readReg(map.phy, 0xbc) & 0xffffU) == 0x3dU) {
					break;
				}
				IODelay(10);
			}
			gSmUfs.kbPokePcs = readReg(map.phy, 0xbc);
			if (icePoke != 0 && gSmUfs.level != 35 && gSmUfs.level != 36) {
				writeReg(icePoke, 0x4, 0x5f);
				gSmUfs.kbPokeIce = readReg(icePoke, 0x4);
			}
		}
		writeReg(hc, SM_UFS_REG_HCE, 1);
		for (unsigned loop = 0; loop < 1000; loop++) {
			if (readReg(hc, SM_UFS_REG_HCE) & 1) {
				break;
			}
			IODelay(10);
		}
		gSmUfs.kbLinkResult = sendUicOpcode(hc, SM_UIC_CMD_DME_LINK_STARTUP,
		                                    gSmUfs.kbLinkHcs, gSmUfs.kbLinkIs);
		gSmUfs.kbIs = readReg(hc, SM_UFS_REG_IS);
		fbFullMark();
	}

	writeReg(hc, 0x24, 0);                            // IE: polled operation
	writeReg(hc, 0x4c, 0);                            // UTRIACR: no interrupt aggregation
	writeReg(hc, SM_UFS_REG_IS, gSmUfs.kbIs);
	// Level 37 never stops the transfer lists: the vendor driver leaves UTRLRSR=1 and its
	// doorbells are answered, so this level isolates whether stopping is what makes the
	// doorbell store never return (HANDOFF-2026-10-30-FULL.md §11).
	if (gSmUfs.level != 37) {
		writeReg(hc, SM_UFS_REG_UTRLRSR, 0);
		writeReg(hc, 0x80, 0);                            // UTMRLRSR: no task management
		bool stopped = false;
		for (unsigned loop = 0; loop < 100000; loop++) {
			if ((readReg(hc, SM_UFS_REG_UTRLRSR) & 1) == 0 && (readReg(hc, 0x80) & 1) == 0 &&
			    readReg(hc, SM_UFS_REG_UTRLDBR) == 0 && readReg(hc, 0x78) == 0) {
				stopped = true;
				break;
			}
			IODelay(1);
		}
		gSmUfs.kbRsr = readReg(hc, SM_UFS_REG_UTRLRSR);
		if (!stopped) {
			report15();
		}
	} else {
		gSmUfs.kbRsr = readReg(hc, SM_UFS_REG_UTRLRSR);
	}
	gSmUfs.kbStep = 2;

	writeReg(hc, SM_UFS_REG_UTRLBA, (uint32_t)dma.phys);
	writeReg(hc, SM_UFS_REG_UTRLBAU, (uint32_t)(dma.phys >> 32));
	// Never ring a doorbell unless the host uses this list.
	if (readReg(hc, SM_UFS_REG_UTRLBA) != (uint32_t)dma.phys ||
	    readReg(hc, SM_UFS_REG_UTRLBAU) != (uint32_t)(dma.phys >> 32)) {
		report15();
	}
	writeReg(hc, SM_UFS_REG_UTRLRSR, 1);
	bool ready = false;
	for (unsigned loop = 0; loop < 100000; loop++) {
		if ((readReg(hc, SM_UFS_REG_UTRLRSR) & 1) != 0 && (readReg(hc, SM_UFS_REG_HCS) & 2) != 0) {
			ready = true;
			break;
		}
		IODelay(1);
	}
	gSmUfs.kbRsr = readReg(hc, SM_UFS_REG_UTRLRSR);
	gSmUfs.kbHcs = readReg(hc, SM_UFS_REG_HCS);
	if (!ready) {
		report15();
	}
	gSmUfs.kbStep = 3;
	if (gSmUfs.level == 21) {
		reportPre21(hc, map.gcc);
	}
	if (gSmUfs.level == 23 || gSmUfs.level == 24) {
		gSmUfs.kbHiberResult = sendUicOpcode(hc, SM_UIC_CMD_DME_HIBER_EXIT,
		                                     gSmUfs.kbHiberHcs, gSmUfs.kbHiberIs);
		if (gSmUfs.level == 24) {
			acknowledgeUicErrors(hc, false);
			reportPre21(hc, map.gcc);
		}
	}
	if (gSmUfs.level >= 16 && gSmUfs.level <= 20) {
		// The three environmental differences still untested, applied before the first fetch.
		// Level 18 drops clk_req, level 19 drops the wildcard, level 20 keeps both but allocates
		// without the 32-bit mask; 17 runs all three.
		gSmUfs.kbCapReg = readReg(hc, SM_UFS_REG_CAP);
		if (gSmUfs.level != 18) {
			gSmUfs.kbClkReqBefore = readReg(hc, 0xfc);
			writeReg(hc, 0xfc, gSmUfs.kbClkReqBefore | 0x3c);   // SYS | ICE | UNIPRO | AUX clk_req
			gSmUfs.kbClkReqAfter = readReg(hc, 0xfc);
		}
		// The descriptor fetch is an unmatched stream unless the firmware left a bypass for it.
		// The display stream (0x820) sits outside the 0x3ff mask and keeps its own translation.
		if (gSmUfs.level != 19) {
			gSmUfs.wildResult = (uint32_t)smmuBypassAnoc1();
		}
	}
	if (gSmUfs.level == 22) {
		ownRingEmpty(hc, dma);
	}
	fbMark(3);
	if (gSmUfs.level == 34) {
		gSmUfs.kbSmmuUfs = smmuBypassUfsStreams();
	}

	// TEST UNIT READY first; the device may report a unit attention right after the handover.
	uint8_t tur[6] = {0};
	for (unsigned attempt = 0; attempt < 3; attempt++) {
		SmUfsCommandResult t = ownSubmit(hc, dma, tur, sizeof(tur), false, 0);
		gSmUfs.kbTurOcs = t.ocs;
		gSmUfs.kbTurRsp = t.response;
		gSmUfs.kbTurScsi = t.scsi;
		gSmUfs.kbTurSense = t.sense;
		gSmUfs.kbTurTries = attempt + 1;
		if (t.ocs != 0 || t.scsi == 0) {
			break;
		}
	}
	gSmUfs.kbStep = 4;
	fbMark(4);
	if ((gSmUfs.kbDbrAfter & 1) != 0) {
		report15();  // the host never finished the first command
	}

	uint8_t capacityCdb[10] = {0x25};
	SmUfsCommandResult capacity = ownSubmit(hc, dma, capacityCdb, sizeof(capacityCdb), true, 8);
	gSmUfs.kbCapOcs = capacity.ocs;
	gSmUfs.kbCapRsp = capacity.response;
	gSmUfs.kbCapScsi = capacity.scsi;
	if (capacity.ocs == 0) {
		gSmUfs.kbCapLast = loadBe32(dma.virt + kKbBounce);
		gSmUfs.kbCapBlk = loadBe32(dma.virt + kKbBounce + 4);
	}
	gSmUfs.kbStep = 5;
	if ((gSmUfs.kbDbrAfter & 1) != 0) {
		report15();
	}

	// READ(10) one 4 KiB block at LBA 0: LUN 0 is the userdata start, so its GPT header sits at
	// byte 512 and proves both the data path and the device mapping.
	uint8_t readCdb[10] = {0x28, 0, 0, 0, 0, 0, 0, 0, 1, 0};
	SmUfsCommandResult read = ownSubmit(hc, dma, readCdb, sizeof(readCdb), true, 4096);
	gSmUfs.kbReadOcs = read.ocs;
	gSmUfs.kbReadRsp = read.response;
	gSmUfs.kbReadScsi = read.scsi;
	uint8_t *data = dma.virt + kKbBounce;
	gSmUfs.kbReadW0 = loadBe32(data + 0);
	gSmUfs.kbReadW1 = loadBe32(data + 4);
	gSmUfs.kbGpt0 = loadBe32(data + 512);
	gSmUfs.kbGpt1 = loadBe32(data + 516);
	gSmUfs.kbStep = 6;
	report15();
	return 0;
}

void NativeUfs::report15(void)
{
	panic("SMUFS kb step=%x phase=%x hce %08x hcs %08x\n"
	      "dbr %08x is %08x>%08x rsr %x tmrl %x fb2 %x\n"
	      "dma %llx low%x c%x cap %08x\n"
	      "clk %08x>%08x wild r%x s%x b%x\n"
	      "smr %08x s2cr %x cbar %x\n"
	      "cold %x link %x hcs %x is %x\n"
	      "hib %x hcs %x is %x\n"
	      "tur %x/%02x/%02x s%03x n%u\n"
	      "cap %x/%02x/%02x lba %08x blk %08x\n"
	      "rd %x/%02x/%02x w %08x %08x gpt %08x %08x\n"
	      "pure list %llx slot %x dw0 %08x\n"
	      "poke cfg1 %08x pcs %08x ice %08x smmu %x\n",
	      gSmUfs.kbStep, gSmUfs.kbPhase, gSmUfs.kbHce, gSmUfs.kbHcs,
	      gSmUfs.kbDbr, gSmUfs.kbIs, gSmUfs.kbIsAfter, gSmUfs.kbRsr, gSmUfs.kbTmrl,
	      gSmUfs.kbFb2,
	      gSmUfs.kbDmaPa, gSmUfs.kbDmaLow, gSmUfs.kbDmaContig, gSmUfs.kbCapReg,
	      gSmUfs.kbClkReqBefore, gSmUfs.kbClkReqAfter,
	      gSmUfs.wildResult, gSmUfs.wildSlot, gSmUfs.wildBank,
	      gSmUfs.wildSmr, gSmUfs.wildS2cr, gSmUfs.wildCbar,
	      gSmUfs.kbColdResult, gSmUfs.kbLinkResult, gSmUfs.kbLinkHcs, gSmUfs.kbLinkIs,
	      gSmUfs.kbHiberResult, gSmUfs.kbHiberHcs, gSmUfs.kbHiberIs,
	      gSmUfs.kbTurOcs, gSmUfs.kbTurRsp, gSmUfs.kbTurScsi, gSmUfs.kbTurSense,
	      gSmUfs.kbTurTries,
	      gSmUfs.kbCapOcs, gSmUfs.kbCapRsp, gSmUfs.kbCapScsi, gSmUfs.kbCapLast, gSmUfs.kbCapBlk,
	      gSmUfs.kbReadOcs, gSmUfs.kbReadRsp, gSmUfs.kbReadScsi,
	      gSmUfs.kbReadW0, gSmUfs.kbReadW1, gSmUfs.kbGpt0, gSmUfs.kbGpt1,
	      gSmUfs.kbUtrlba, gSmUfs.kbPureSlot, gSmUfs.kbPureDw0,
	      gSmUfs.kbPokeCfg1, gSmUfs.kbPokePcs, gSmUfs.kbPokeIce, gSmUfs.kbSmmuUfs);
}

void NativeUfs::reportPre21(uintptr_t hc, uintptr_t gcc)
{
	OSSharedPtr<IOMemoryMap> iceMap;
	uintptr_t ice = mapPhys(SM_UFS_ICE_BASE, SM_UFS_ICE_SIZE, iceMap);
	SmUfsRcg axi, unip, iceRcg, aux;
	readRcg(gcc, SM_GCC_UFS_AXI_RCG, axi);
	readRcg(gcc, SM_GCC_UFS_UNIPRO_RCG, unip);
	readRcg(gcc, SM_GCC_UFS_ICE_RCG, iceRcg);
	readRcg(gcc, SM_GCC_UFS_PHY_AUX_RCG, aux);
	panic("SMUFS pre21 hce %08x hcs %08x cap %08x\n"
	      "is %08x dbr %08x rsr %08x utrlba %08x\n"
	      "bau %08x gdsc %08x cfg1 %08x cfg2 %08x\n"
	      "ah8 %08x t18 %08x\n"
	      "cbc axi %08x ahb %08x tx0 %08x\n"
	      "cbc rx0 %08x rx1 %08x unip %08x\n"
	      "cbc ice %08x aux %08x agg %08x\n"
	      "rcg axi cmd %08x cfg %08x m %x n %x d %x\n"
	      "rcg unip cmd %08x cfg %08x m %x n %x d %x\n"
	      "rcg ice cmd %08x cfg %08x m %x n %x d %x\n"
	      "rcg aux cmd %08x cfg %08x m %x n %x d %x\n"
	      "ice ver %x ctrl %x byp %x\n"
	      "hib %x hcs %x is %x\n"
	      "uic is %x>%x ret %x err %x %x %x %x %x\n"
	      "pre ba %x is %x hcs %x hce %x\n",
	      readReg(hc, SM_UFS_REG_HCE), readReg(hc, SM_UFS_REG_HCS), readReg(hc, SM_UFS_REG_CAP),
	      readReg(hc, SM_UFS_REG_IS), readReg(hc, SM_UFS_REG_UTRLDBR), readReg(hc, SM_UFS_REG_UTRLRSR),
	      readReg(hc, SM_UFS_REG_UTRLBA),
	      readReg(hc, SM_UFS_REG_UTRLBAU), readReg(gcc, (uint32_t)(SM_UFS_PHY_GDSC - SM_GCC_BASE)),
	      readReg(hc, SM_UFS_REG_CFG1), readReg(hc, SM_UFS_REG_CFG2),
	      readReg(hc, 0xfc), readReg(hc, 0x18),
	      readReg(gcc, SM_GCC_UFS_AXI_CBCR), readReg(gcc, SM_GCC_UFS_AHB_CBCR),
	      readReg(gcc, SM_GCC_UFS_TX_SYMBOL0_CBCR),
	      readReg(gcc, SM_GCC_UFS_RX_SYMBOL0_CBCR), readReg(gcc, SM_GCC_UFS_RX_SYMBOL1_CBCR),
	      readReg(gcc, SM_GCC_UFS_UNIPRO_CORE_CBCR),
	      readReg(gcc, SM_GCC_UFS_ICE_CORE_CBCR), readReg(gcc, SM_GCC_UFS_PHY_AUX_CBCR),
	      readReg(gcc, SM_GCC_AGGRE_UFS_AXI_CBCR),
	      axi.cmd, axi.cfg, axi.m, axi.n, axi.d,
	      unip.cmd, unip.cfg, unip.m, unip.n, unip.d,
	      iceRcg.cmd, iceRcg.cfg, iceRcg.m, iceRcg.n, iceRcg.d,
	      aux.cmd, aux.cfg, aux.m, aux.n, aux.d,
	      ice ? readReg(ice, 0x008) : 0xdead, ice ? readReg(ice, 0x000) : 0xdead,
	      ice ? readReg(ice, 0x074) : 0xdead,
	      gSmUfs.kbHiberResult, gSmUfs.kbHiberHcs, gSmUfs.kbHiberIs,
	      gSmUfs.uicIsBefore, gSmUfs.uicIsAfter, gSmUfs.uicReturns,
	      gSmUfs.uicErr[0], gSmUfs.uicErr[1], gSmUfs.uicErr[2], gSmUfs.uicErr[3],
	      gSmUfs.uicErr[4],
	      gSmUfs.pre.utrlba, gSmUfs.pre.is, gSmUfs.pre.hcs, gSmUfs.pre.hce);
}

// One panic, both snapshots. Three label/value triplets per line, ~50 columns: the panic screen
// is a narrow framebuffer console.
void NativeUfs::report(void)
{
	SmUfsSnapshot &a = gSmUfs.pre;
	SmUfsSnapshot &b = gSmUfs.post;

	panic("SMUFS p%x stage=%x r=%x pcs=%u/%u\n"
	      "gdsc %08x>%08x axi %08x>%08x\n"
	      "ahb %08x>%08x unip %08x>%08x\n"
	      "agg %08x>%08x aux %08x>%08x\n"
	      "ice %08x>%08x tx0 %08x>%08x\n"
	      "rx0 %08x>%08x rx1 %08x>%08x\n"
	      "cap %08x ver %08x hcs %08x\n"
	      "hce %08x rsr %08x dbr %08x\n"
	      "utrlba %08x is %08x\n"
	      "cfg1 %08x>%08x\n"
	      "phy %08x %08x %08x\n"
	      "pcs %08x>%08x lane %08x\n"
	      "smmu %08x %08x idr %08x %08x n %u\n"
	      "s60 idx %x smr %08x s2cr %08x\n"
	      "smr1 idx %x sid %x mask %x s2cr %x cbar %x sctlr %x ttbr0 %x\n",
	      gSmUfs.level, gSmUfs.stage, gSmUfs.result, gSmUfs.phyResult, gSmUfs.pcsPolls,
	      a.gdsc, b.gdsc, a.axi, b.axi,
	      a.ahb, b.ahb, a.unipro, b.unipro,
	      a.aggre, b.aggre, a.aux, b.aux,
	      a.ice, b.ice, a.tx0, b.tx0,
	      a.rx0, b.rx0, a.rx1, b.rx1,
	      a.cap, a.ver, a.hcs,
	      a.hce, a.rsr, a.dbr,
	      a.utrlba, a.is,
	      a.cfg1, b.cfg1,
	      b.phyStart, b.phyPower, b.phyReset,
	      a.pcs, b.pcs, b.laneCfg,
	      gSmUfs.smmuScr0, gSmUfs.smmuAcr, gSmUfs.smmuIdr0, gSmUfs.smmuIdr1, gSmUfs.smmuValid,
	      gSmUfs.smmuIdx60, gSmUfs.smmuS60, gSmUfs.smmuS2c60,
	      gSmUfs.smmuIdx1, gSmUfs.smmuSid1, gSmUfs.smmuMask1, gSmUfs.smmuS2cr1,
	      gSmUfs.smmuCbar1, gSmUfs.smmuSctlr1, gSmUfs.smmuTtbr01);
}

// Stage 3 report: the takeover result and the identification commands. One line per stage so a
// photographed screen can be read back without the register dump above it.
void NativeUfs::report3(void)
{
	panic("SMUFS p3 take=%x r=%x prog=%x stage=%x dma=%08x\n"
	      "cap %08x ver %08x hcs %08x hce %08x\n"
	      "rsr %08x>%08x hcs2 %08x dbr %08x\n"
	      "nop ocs %x rsp %02x scsi %02x\n"
	      "wake ocs %x rsp %02x scsi %02x\n"
	      "cap10 ocs %x rsp %02x scsi %02x\n"
	      "lba %08x blk %08x\n"
	      "read ocs %x rsp %02x scsi %02x\n"
	      "w0 %08x w1 %08x\n"
	      "gpt %08x %08x\n"
	      "smmu %08x %08x idr %08x %08x n %u\n"
	      "s60 idx %x smr %08x s2cr %08x\n"
	      "smr1 idx %x sid %x mask %x s2cr %x cbar %x sctlr %x ttbr0 %x\n"
	      "utrd %08x %08x %08x %08x %08x %08x %08x %08x\n"
	      "uic arg3 %08x hcs %08x\n"
	      "clk %08x %08x %08x %08x\n"
	      "smmu insp %x first sid %x s2cr %08x fsr %08x gfsr %08x scr0 %x\n"
	      "ice %x %x %x %x cfg2 %x ah8 %x\n"
	      "clkreq %08x\n"
	      "qpro hwver %08x cfg1 %08x>%08x iceadv %08x>%08x bist %08x polls %x\n"
	      "rcg cmd %08x %08x %08x %08x\n"
	      "rcg late %08x %08x %08x %08x\n"
	      "rcg fix %x %x %x %x forced %x upd %x %x %x %x\n"
	      "rcg polls %x %x %x %x\n"
	      "uic ack %08x>%08x ret %x\n"
	      "uicerr %x %x %x %x %x\n",
	      gSmUfs.takeResult, gSmUfs.result, gSmUfs.progress, gSmUfs.stage, gSmUfs.dmaPhys,
	      gSmUfs.pre.cap, gSmUfs.pre.ver, gSmUfs.pre.hcs, gSmUfs.pre.hce,
	      gSmUfs.rsrBefore, gSmUfs.rsrAfter, gSmUfs.hcsAfter, gSmUfs.dbrAfter,
	      gSmUfs.nopOcs, gSmUfs.nopResp, gSmUfs.nopScsi,
	      gSmUfs.wakeOcs, gSmUfs.wakeResp, gSmUfs.wakeScsi,
	      gSmUfs.capOcs, gSmUfs.capResp, gSmUfs.capScsi,
	      gSmUfs.capLastLba, gSmUfs.capBlockLen,
	      gSmUfs.readOcs, gSmUfs.readResp, gSmUfs.readScsi,
	      gSmUfs.readW0, gSmUfs.readW1,
	      gSmUfs.readGpt0, gSmUfs.readGpt1,
	      gSmUfs.smmuScr0, gSmUfs.smmuAcr, gSmUfs.smmuIdr0, gSmUfs.smmuIdr1, gSmUfs.smmuValid,
	      gSmUfs.smmuIdx60, gSmUfs.smmuS60, gSmUfs.smmuS2c60,
	      gSmUfs.smmuIdx1, gSmUfs.smmuSid1, gSmUfs.smmuMask1, gSmUfs.smmuS2cr1,
	      gSmUfs.smmuCbar1, gSmUfs.smmuSctlr1, gSmUfs.smmuTtbr01,
	      gSmUfs.utrdCheck[0], gSmUfs.utrdCheck[1], gSmUfs.utrdCheck[2], gSmUfs.utrdCheck[3],
	      gSmUfs.utrdCheck[4], gSmUfs.utrdCheck[5], gSmUfs.utrdCheck[6], gSmUfs.utrdCheck[7],
	      gSmUfs.uicResult, gSmUfs.uicHcs,
	      gSmUfs.clkAxi, gSmUfs.clkAgg, gSmUfs.clkUnip, gSmUfs.clkIce,
	      gSmUfs.bypassResult, gSmUfs.bypassSid, gSmUfs.bypassS2crBefore,
	      gSmUfs.bypassFsr, gSmUfs.bypassGfsr, gSmUfs.bypassScr0,
	      gSmUfs.iceVersion, gSmUfs.iceCtrl, gSmUfs.iceBypass, gSmUfs.iceBist,
	      gSmUfs.cfg2, gSmUfs.ah8Timer,
	      gSmUfs.ah8Cfg,
	      gSmUfs.hwVersion, gSmUfs.cfg1Before, gSmUfs.cfg1After,
	      gSmUfs.iceAdvBefore, gSmUfs.iceAdvAfter, gSmUfs.iceBistFinal,
	      gSmUfs.iceBistPolls,
	      gSmUfs.rcgBefore[0].cmd, gSmUfs.rcgBefore[1].cmd, gSmUfs.rcgBefore[2].cmd,
	      gSmUfs.rcgBefore[3].cmd,
	      gSmUfs.rcgLate[0].cmd, gSmUfs.rcgLate[1].cmd, gSmUfs.rcgLate[2].cmd,
	      gSmUfs.rcgLate[3].cmd,
	      gSmUfs.rcgFix[0], gSmUfs.rcgFix[1], gSmUfs.rcgFix[2], gSmUfs.rcgFix[3],
	      gSmUfs.rcgForced,
	      gSmUfs.rcgUpdate[0], gSmUfs.rcgUpdate[1], gSmUfs.rcgUpdate[2], gSmUfs.rcgUpdate[3],
	      gSmUfs.rcgFixPolls[0], gSmUfs.rcgFixPolls[1], gSmUfs.rcgFixPolls[2],
	      gSmUfs.rcgFixPolls[3],
	      gSmUfs.uicIsBefore, gSmUfs.uicIsAfter, gSmUfs.uicReturns,
	      gSmUfs.uicErr[0], gSmUfs.uicErr[1], gSmUfs.uicErr[2], gSmUfs.uicErr[3],
	      gSmUfs.uicErr[4]);
}

// Level 9 report: the ShengUfs minimal-adoption result. One line per stage so a photographed
// screen tells which guard or step stopped.
void NativeUfs::report9(void)
{
	panic("SMUFS min step=%x r=%x list=%llx\n"
	      "hcs %08x>%08x hce %08x dbr %08x>%08x\n"
	      "rsr %08x>%08x is %08x>%08x tmrl %08x\n"
	      "nop ocs %x rsp %02x scsi %02x polls %x fatal %x\n"
	      "wild r %x slot %x bank %x smr %x s2cr %x cbar %x cba2r %x sctlr %x\n",
	      gSmUfs.minStep, gSmUfs.result, gSmUfs.minList,
	      gSmUfs.minHcsBefore, gSmUfs.minHcsAfter, gSmUfs.minHce,
	      gSmUfs.minDbrBefore, gSmUfs.minDbrAfter,
	      gSmUfs.minRsrBefore, gSmUfs.minRsrAfter,
	      gSmUfs.minIsBefore, gSmUfs.minIsAfter, gSmUfs.minTmrlBefore,
	      gSmUfs.minOcs, gSmUfs.minRsp, gSmUfs.minScsi, gSmUfs.minPolls,
	      gSmUfs.minFatal,
	      gSmUfs.wildResult, gSmUfs.wildSlot, gSmUfs.wildBank, gSmUfs.wildSmr,
	      gSmUfs.wildS2cr, gSmUfs.wildCbar, gSmUfs.wildCba2r, gSmUfs.wildSctlr);
}

// Level 12: report the transport state measured just before the doorbell, then panic instead of
// ringing. The panic screen is the only output channel that has been reliable in these runs; if
// the NoC QoS read itself hung, this panic never prints, which is itself the answer.
void NativeUfs::report12(void)
{
	panic("SMUFS transp noc prio %08x mode %08x bw %08x sat %08x\n"
	      "aggre %08x pwr done %x res %x mode %08x\n"
	      "list %llx hcs %08x dbr %08x rsr %08x is %08x\n",
	      gSmUfs.nocPrio, gSmUfs.nocMode, gSmUfs.nocBw, gSmUfs.nocSat,
	      gSmUfs.aggreCbcr, gSmUfs.pwrDone, gSmUfs.pwrRes, gSmUfs.pwrMode,
	      gSmUfs.minList, readReg(gSmUfs.hcBase, SM_UFS_REG_HCS),
	      readReg(gSmUfs.hcBase, SM_UFS_REG_UTRLDBR),
	      readReg(gSmUfs.hcBase, SM_UFS_REG_UTRLRSR),
	      readReg(gSmUfs.hcBase, SM_UFS_REG_IS));
}

// H1: the service stays registered and online while BSD root matching runs.
// This report is taken after a wait well past the v23 terminal deadline, so a
// successful late read is evidence of a persistent provider, not a one-shot test.
void NativeUfs::productionReport(void)
{
	using namespace SmUfsRootProduction;
	IOSleep(20000);
	IOLockLock(blockStartupGate);
	const bool late = !__atomic_load_n(&blockCancelled, __ATOMIC_ACQUIRE) && lateRead();
	snapshot();
	const Report r = gReport;
	const auto systemWindow = SmUfsRootWindows::entries[0];
	const auto osWindow = SmUfsRootWindows::entries[1];
	IOLockUnlock(blockStartupGate);
	gSmUfs.progressDone = 1;
	uint32_t rootMode = 1;
	(void)PE_parse_boot_argn("native-ufs-root", &rootMode, sizeof(rootMode));
	// H3P: only the explicit continuous mode skips this planned observation
	// panic. This is provider health, not root-authentication/cache permission.
	if (rootMode == 2 && SmUfsRootWindows::enabled() &&
	    !__atomic_load_n(&blockCancelled, __ATOMIC_ACQUIRE) &&
	    r.registered && r.online && r.readOnly && r.geometry && r.whole &&
	    r.manifestValid && r.windowsPresent && r.probeComplete && late && r.lateStable &&
	    systemWindow.registered && systemWindow.geometry && osWindow.registered && osWindow.geometry) {
		SMUFS_LOG("root-h3p observation complete; provider remains online");
		return;
	}
	if (SmUfsRootWindows::enabled()) {
		static SmUfsRootTraceSnapshot observed;
		const uint32_t coherent = native_smufs_root_trace_snapshot(&observed);
		const auto *t = observed.records;
		panic("SMUFS root-h2 level71 phase=observation reason=%s stage=%u\n"
		      "windows sys %llx/%llx reg%u geo%u os %llx/%llx reg%u geo%u\n"
		      "h1 probe%u late%u stable%u on%u req%u/%u rej%u sub%u infl%u\n"
		      "trace en%u coherent%u partial%u lost%u writes%u\n"
		      "find%u/%u match%u inputs%x wait%u/%u present%d\n"
		      "selected%u valid%u dev%x properties%x mount%u/%u\n",
		      r.reason, r.stage, systemWindow.mediaID, systemWindow.driverID,
		      systemWindow.registered ? 1u : 0u, systemWindow.geometry ? 1u : 0u,
		      osWindow.mediaID, osWindow.driverID, osWindow.registered ? 1u : 0u, osWindow.geometry ? 1u : 0u,
		      r.probeComplete ? 1u : 0u, late ? 1u : 0u, r.lateStable ? 1u : 0u, r.online ? 1u : 0u,
		      r.accepted, r.completed, r.rejected, r.submits, r.inflight,
		      observed.enabled, coherent, observed.partial, observed.lost, observed.writes,
		      t[SMRT_FIND_ENTER].count, t[SMRT_FIND_RETURN].count, t[SMRT_MATCH_SOURCE].arg, t[SMRT_INPUTS].arg,
		      t[SMRT_ROOT_WAIT_ENTER].count, t[SMRT_ROOT_WAIT_RETURN].count, t[SMRT_ROOT_WAIT_RETURN].result,
		      t[SMRT_SELECTED].count, t[SMRT_SELECTED].dev_valid, t[SMRT_SELECTED].dev,
		      t[SMRT_BSD_PROPERTIES].arg, t[SMRT_MOUNT_ENTER].count, t[SMRT_MOUNT_RETURN].count);
	}
	panic("SMUFS root-h1 level71 stage=%x reason=%s\n"
	      "gpt user %llx..%llx\n"
	      "media reg %llx/%llx blk %u blocks %llx whole%u ro%u on%u geo%u\n"
	      "manifest v%u n%u ok%u err%u bytes %llx\n"
	      "win OS %llx+%llx sys %llx+%llx\n"
	      "os %08x %08x apfs %08x tail %08x sys %08x %08x\n"
	      "probe%u late%u stable%u via-media%u reg-state%u\n"
	      "req %u/%u rej%u sub%u infl%u\n",
	      r.stage, r.reason, r.userFirst, r.userLast,
	      r.mediaRegistryID, r.driverRegistryID, r.blockBytes, r.blocks,
	      r.whole ? 1u : 0u, r.readOnly ? 1u : 0u, r.online ? 1u : 0u, r.geometry ? 1u : 0u,
	      r.version, r.count, r.manifestValid ? 1u : 0u, r.parseError, r.imageBytes,
	      r.windowOffsets[0], r.windowSizes[0], r.windowOffsets[1], r.windowSizes[1],
	      r.osWords[0], r.osWords[1], r.osApfsWord, r.osTailWord, r.systemWords[0], r.systemWords[1],
	      r.probeComplete ? 1u : 0u, late ? 1u : 0u, r.lateStable ? 1u : 0u,
	      r.lateViaMedia ? 1u : 0u, r.registered ? 1u : 0u,
	      r.accepted, r.completed, r.rejected, r.submits, r.inflight);
}

void NativeUfs::stop(IOService *provider)
{
	__atomic_store_n(&blockCancelled, 1, __ATOMIC_RELEASE);
	if (blockStartupGate) {
		IOLockLock(blockStartupGate);
		if (gSmUfs.level == 71) (void)SmUfsRootProduction::stop();
		if (blockContext && blockContext->gate) {
			blockContext->lock();
			blockContext->stopping = true;
			blockContext->online = false;
			blockContext->unlock();
		}
		IOLockUnlock(blockStartupGate);
		// Probe65 retains all hardware-referenced memory until its terminal report.
	}
	IOService::stop(provider);
}
