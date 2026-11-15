/*
 * The touch screen of the OnePlus 8 Pro (in2020): a Samsung S6SY791 on the I2C bus of serial
 * engine 13 (qupv3_se13 at 0xa94000), ported from the hakios in2020 port, whose driver was
 * verified on this phone (device id 0x3791, panel 1440 x 3168, 17 channels).
 *
 *   bus       i2c@a94000, address 0x48, pins gpio36 (data) and gpio37 (clock), driven by hand
 *   reset     gpio38 (active low)
 *   interrupt gpio39 (input, low while an event is queued)
 *   supplies  pm8150_l13 = 3.008 V ("ldoa13") and pm8150a_l1 = 1.8 V ("ldoc1") over RPMh
 *
 * Contacts are published to the native_touch_* seqlock that the framework HID component
 * (NativeHidInput.cpp) reads - the same contract the reference TouchProbe.cpp fulfils.
 * Boot argument touchscreen=1 starts it.
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>


extern "C" {
extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);
}

extern volatile uint32_t native_touch_seq, native_touch_count;
extern "C" void native_touch_notify(void);
extern volatile int native_touch_xs[12], native_touch_ys[12], native_touch_ids[12];

#define HAKIOS_TOUCH_UP   1
#define HAKIOS_TOUCH_DOWN 2
#define HAKIOS_TOUCH_MOVE 3

static lck_mtx_t *s6_lock;
static bool s6_down[12];
static int s6_x[12], s6_y[12];


/* Rebuild the whole contact set from this driver's own table, so a lifting finger always leaves a
 * consistent frame behind: odd seq while writing, even when stable. */
static void
hakios_touch_post(uint32_t type, uint32_t finger, uint32_t x, uint32_t y)
{
	int nextX[12], nextY[12], ids[12];
	uint32_t count = 0;

	if (finger >= 12) {
		return;
	}
	if (type == HAKIOS_TOUCH_DOWN) {
		s6_down[finger] = true;
		s6_x[finger] = (int)x;
		s6_y[finger] = (int)y;
	} else if (type == HAKIOS_TOUCH_MOVE) {
		s6_x[finger] = (int)x;
		s6_y[finger] = (int)y;
	} else if (type == HAKIOS_TOUCH_UP) {
		s6_down[finger] = false;
	}
	for (unsigned f = 0; f < 12; f++) {
		if (!s6_down[f]) continue;
		nextX[count] = s6_x[f];
		nextY[count] = s6_y[f];
		ids[count] = (int)f + 1;
		count = count + 1;
	}
	native_touch_seq = native_touch_seq + 1;
	for (unsigned i = 0; i < count; i++) {
		native_touch_xs[i] = nextX[i];
		native_touch_ys[i] = nextY[i];
		native_touch_ids[i] = ids[i];
	}
	native_touch_count = count;
	native_touch_seq = native_touch_seq + 1;
	native_touch_notify();
}
static vm_offset_t tlmm, rsc, cmd_db;

static inline uint32_t
rd(vm_offset_t address)
{
	return *(volatile uint32_t *)address;
}

static inline void
wr(vm_offset_t address, uint32_t value)
{
	*(volatile uint32_t *)address = value;
}

static void
sleep_us(uint32_t us)
{
	delay_for_interval(us, NSEC_PER_USEC);
}

static void
sleep_ms(uint32_t ms)
{
	delay_for_interval(ms, NSEC_PER_MSEC);
}

/* The bit-banged bus counts its half bits itself: delay_for_interval is a whole sleep, this is a look at the timer. */
static void
spin_us(uint32_t us)
{
	uint64_t until;

	nanoseconds_to_absolutetime((uint64_t)us * NSEC_PER_USEC, &until);
	until += mach_absolute_time();
	while (mach_absolute_time() < until) {
	}
}

/* ---- pins (TLMM) ------------------------------------------------------------------------------ */

/*
 * The pins. SM8250's TLMM is three tiles -- west 0x0f100000, south 0x0f500000, north 0x0f900000, 0x300000
 * each (drivers/pinctrl/qcom/pinctrl-sm8250.c: gpio36..gpio39 are SOUTH) -- and the phone's tree gives the
 * three as one node, /soc/pinctrl@f000000, 0x1000000 long. A pad's registers are at its tile + 0x1000 * its
 * number: gpio36 is 0x0f500000 + 0x24000.
 */
#define TLMM_PHYS               0x0f500000UL     /* the south tile: every pin this driver uses is in it */
#define TLMM_SIZE               0x100000UL       /* 256 pads' registers are enough for the four that matter */
#define GPIO_CFG(n)             (tlmm + 0x1000UL * (n))         /* [1:0] pull, [5:2] function, [8:6] drive, [9] output */
#define GPIO_IO(n)              (tlmm + 0x1000UL * (n) + 4)     /* bit 0 in, bit 1 out */
#define CFG_PULL_UP             3
#define CFG_OUTPUT              (1u << 9)

#define PIN_SDA                 36      /* qupv3_se13_i2c: the first pin is the data line ... */
#define PIN_SCL                 37      /* ... and the second the clock */
#define PIN_RESET               38
#define PIN_INT                 39

static void
show_pins(const char *when)
{
	static const struct {
		unsigned pin;
		const char *what;
	} pins[] = { { PIN_SDA, "I2C data" }, { PIN_SCL, "I2C clock" }, { PIN_RESET, "reset" }, { PIN_INT, "interrupt" } };

	for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
		uint32_t cfg = rd(GPIO_CFG(pins[i].pin)), io = rd(GPIO_IO(pins[i].pin));

		printf("s6sy791: %s: gpio%u (%s): cfg %#x io %#x: function %u, pull %u, %u mA, %s, reads %u\n", when,
		    pins[i].pin, pins[i].what, cfg, io, (cfg >> 2) & 15, cfg & 3, (((cfg >> 6) & 7) + 1) * 2,
		    (cfg & CFG_OUTPUT) ? (io & 2 ? "drives high" : "drives low") : "input", io & 1);
	}
}

/* the reset line as an output of the GPIO function (2 mA, no pull, as the tree has it) */
static void
reset_pin(bool high)
{
	wr(GPIO_IO(PIN_RESET), high ? 2 : 0);
	wr(GPIO_CFG(PIN_RESET), CFG_OUTPUT);
}

/* the interrupt line as an input with its pull-up */
static void
int_pin_input(void)
{
	wr(GPIO_CFG(PIN_INT), CFG_PULL_UP);
}

static bool
int_low(void)
{
	return !(rd(GPIO_IO(PIN_INT)) & 1);
}

/* touch_probe=2: the four pads are read and the driver stops before it touches anything. */
static bool pads_only;

/* ---- the two supplies (RPMh) ------------------------------------------------------------------- */

#define CMD_DB_PHYS             0x80860000UL    /* the phone's tree: cmd-db 0x80860000 + 0x20000 */
#define CMD_DB_SIZE             0x20000UL
#define RSC_PHYS                0x18220000UL    /* the application processor's "DRV 2" of the RSC at 0x18200000 */
#define RSC_SIZE                0x10000UL
#define TCS(reg)                (rsc + 0xd00 + (reg))           /* command slot set 0, an "active" one */
#define TCS_IRQ_ENABLE          0x00
#define TCS_IRQ_STATUS          0x04
#define TCS_IRQ_CLEAR           0x08
#define TCS_WAIT_FOR_COMPLETION 0x10
#define TCS_CONTROL             0x14
#define TCS_CONTROL_ENABLE      (1u << 16)
#define TCS_CONTROL_TRIGGER     (1u << 24)
#define TCS_STATUS              0x18            /* 0 while busy */
#define TCS_CMD_ENABLE          0x1c
#define TCS_CMD_MSGID(n)        (0x30 + 20 * (n))
#define TCS_CMD_ADDR(n)         (0x34 + 20 * (n))
#define TCS_CMD_DATA(n)         (0x38 + 20 * (n))
#define MSGID_WRITE_4_BYTES     0x10008
#define MSGID_RESPONSE          0x100

#define VRM_VOLTAGE             0               /* a regulator's registers, from its address: millivolts */
#define VRM_ENABLE              4
#define VRM_MODE                8
#define VRM_MODE_LDO_HPM        7

struct cmd_db_slave {
	uint16_t id, header_offset, data_offset, count, version, reserved[3];
};
struct cmd_db_header {
	uint32_t version;
	uint8_t magic[4];
	struct cmd_db_slave slave[8];
	uint32_t checksum, reserved;
	uint8_t data[];
};
struct cmd_db_entry {
	char id[8];
	uint32_t priority[2];
	uint32_t address;
	uint16_t length, offset;
};

/* The RPMh address of a resource by its name in the command database the boot loader left; 0 = none. */
static uint32_t
rpmh_address(const char *name)
{
	const struct cmd_db_header *db = (const struct cmd_db_header *)cmd_db;
	char id[8] = { 0 };

	if (db->magic[0] != 0xdb || db->magic[1] != 0x30 || db->magic[2] != 0x03 || db->magic[3] != 0x0c) {
		printf("s6sy791: no command database (magic %02x %02x %02x %02x)\n", db->magic[0], db->magic[1],
		    db->magic[2], db->magic[3]);
		return 0;
	}
	strlcpy(id, name, sizeof(id));
	for (unsigned s = 0; s < 8; s++) {
		const struct cmd_db_slave *slave = &db->slave[s];
		const struct cmd_db_entry *entry = (const struct cmd_db_entry *)(db->data + slave->header_offset);

		if (slave->id == 0 || slave->header_offset >= CMD_DB_SIZE - 0x1000 || slave->count > 512) {
			continue;
		}
		for (unsigned i = 0; i < slave->count; i++) {
			if (memcmp(entry[i].id, id, sizeof(id)) == 0) {
				return entry[i].address;
			}
		}
	}
	return 0;
}

static bool
rpmh_wait(uint32_t reg, uint32_t mask, uint32_t value, const char *what)
{
	for (int i = 0; i < 2000; i++) {
		if ((rd(TCS(reg)) & mask) == value) {
			return true;
		}
		sleep_us(100);
	}
	printf("s6sy791: RPMh: timeout waiting for %s (register %#x = %#x)\n", what, reg, rd(TCS(reg)));
	return false;
}

/* Send up to three register writes to RPMh and wait until they are done. */
static bool
rpmh_write(const uint32_t *address, const uint32_t *data, unsigned count)
{
	uint32_t irq_enable;
	bool done;

	lck_mtx_lock(s6_lock);
	irq_enable = rd(TCS(TCS_IRQ_ENABLE));
	/* the slot set must be idle, and switched off the way the hardware wants it: trigger, then enable */
	for (int i = 0; rd(TCS(TCS_STATUS)) == 0; i++) {
		if (i > 2000) {
			printf("s6sy791: RPMh: the command slots stay busy\n");
			lck_mtx_unlock(s6_lock);
			return false;
		}
		sleep_us(100);
	}
	wr(TCS(TCS_CONTROL), rd(TCS(TCS_CONTROL)) & ~TCS_CONTROL_TRIGGER);
	wr(TCS(TCS_CONTROL), rd(TCS(TCS_CONTROL)) & ~TCS_CONTROL_ENABLE);
	wr(TCS(TCS_IRQ_CLEAR), 1);
	for (unsigned i = 0; i < count; i++) {
		wr(TCS(TCS_CMD_MSGID(i)), MSGID_WRITE_4_BYTES | MSGID_RESPONSE);
		wr(TCS(TCS_CMD_ADDR(i)), address[i]);
		wr(TCS(TCS_CMD_DATA(i)), data[i]);
	}
	wr(TCS(TCS_WAIT_FOR_COMPLETION), (1u << count) - 1);
	wr(TCS(TCS_CMD_ENABLE), (1u << count) - 1);
	wr(TCS(TCS_IRQ_ENABLE), irq_enable | 1);
	wr(TCS(TCS_CONTROL), TCS_CONTROL_ENABLE);
	wr(TCS(TCS_CONTROL), TCS_CONTROL_ENABLE | TCS_CONTROL_TRIGGER);

	done = rpmh_wait(TCS_IRQ_STATUS, 1, 1, "the request");
	wr(TCS(TCS_CONTROL), rd(TCS(TCS_CONTROL)) & ~TCS_CONTROL_TRIGGER);
	wr(TCS(TCS_CONTROL), rd(TCS(TCS_CONTROL)) & ~TCS_CONTROL_ENABLE);
	wr(TCS(TCS_CMD_ENABLE), 0);
	wr(TCS(TCS_WAIT_FOR_COMPLETION), 0);
	wr(TCS(TCS_IRQ_CLEAR), 1);
	wr(TCS(TCS_IRQ_ENABLE), irq_enable);
	lck_mtx_unlock(s6_lock);
	return done;
}

/*
 * One regulator on: its voltage, then its enable bit, then its mode (the same three writes, in the
 * same order, as hakios.goodix.c makes for the panel's I/O rail). The first name is what the
 * command database should hold; when the chip does not take that name, the same name without
 * the PMIC's letter is tried, and what answered is said.
 */
static bool
regulator_on(const char *name, const char *other, uint32_t millivolts)
{
	uint32_t base = rpmh_address(name);
	uint32_t address[3], data[3];

	if (base == 0 && other != NULL) {
		base = rpmh_address(other);
		if (base != 0) {
			printf("s6sy791: RPMh: no regulator \"%s\"; \"%s\" is there (address %#x)\n", name, other, base);
		}
	}
	/* the addresses of regulators ("VRM") have 4 in bits 19:16; anything else is not what we are looking for */
	if (base == 0 || (base >> 16 & 0xf) != 4) {
		printf("s6sy791: RPMh: no regulator \"%s\" (address %#x)\n", name, base);
		return false;
	}
	address[0] = base + VRM_VOLTAGE;
	address[1] = base + VRM_ENABLE;
	address[2] = base + VRM_MODE;
	data[0] = millivolts;
	data[1] = 1;
	data[2] = VRM_MODE_LDO_HPM;
	if (!rpmh_write(address, data, 3)) {
		return false;
	}
	printf("s6sy791: %s (%#x): %u mV, enabled, high-power mode\n", name, base, millivolts);
	return true;
}

/* ---- I2C by hand (as in hakios.haptic.c and hakios.speaker.c) ----------------------------------- */

static uint32_t i2c_half_us = 5;        /* touch_i2c_us: 5 us = 100 kHz */

static void
half_bit(void)
{
	spin_us(i2c_half_us);
}

static void
line(unsigned pin, bool high)
{
	wr(GPIO_IO(pin), 0);
	wr(GPIO_CFG(pin), CFG_PULL_UP | (high ? 0 : CFG_OUTPUT));
}

static bool
level_of(unsigned pin)
{
	return rd(GPIO_IO(pin)) & 1;
}

static void
i2c_start(void)
{
	line(PIN_SDA, true);
	line(PIN_SCL, true);
	half_bit();
	line(PIN_SDA, false);
	half_bit();
	line(PIN_SCL, false);
	half_bit();
}

static void
i2c_stop(void)
{
	line(PIN_SDA, false);
	half_bit();
	line(PIN_SCL, true);
	half_bit();
	line(PIN_SDA, true);
	half_bit();
}

static bool
i2c_bit(bool out)
{
	bool in;

	line(PIN_SDA, out);
	half_bit();
	line(PIN_SCL, true);
	for (unsigned stretched = 0; !level_of(PIN_SCL) && stretched < 100; stretched++) {
		half_bit();
	}
	half_bit();
	in = level_of(PIN_SDA);
	line(PIN_SCL, false);
	return in;
}

static bool
i2c_write_byte(uint8_t byte)
{
	for (int bit = 7; bit >= 0; bit--) {
		i2c_bit((byte >> bit) & 1);
	}
	return !i2c_bit(true);          /* the chip pulls the line low for its acknowledgement */
}

static uint8_t
i2c_read_byte(bool more)
{
	uint8_t byte = 0;

	for (int bit = 7; bit >= 0; bit--) {
		byte = (uint8_t)(byte << 1 | i2c_bit(true));
	}
	i2c_bit(!more);
	return byte;
}

static uint32_t taken_cfg[2], taken_io[2];

static bool
bus_take(void)
{
	taken_cfg[0] = rd(GPIO_CFG(PIN_SDA));
	taken_cfg[1] = rd(GPIO_CFG(PIN_SCL));
	taken_io[0] = rd(GPIO_IO(PIN_SDA));
	taken_io[1] = rd(GPIO_IO(PIN_SCL));
	line(PIN_SDA, true);
	line(PIN_SCL, true);
	/*
	 * The chip may still be holding the clock down from the transfer before this one (a clock
	 * stretch that outlives the stop): give it up to 20 ms before deciding the bus is not ours.
	 */
	for (unsigned waited = 0; waited < 4000; waited++) {
		if (level_of(PIN_SDA) && level_of(PIN_SCL)) {
			break;
		}
		half_bit();
	}
	if (!level_of(PIN_SDA) || !level_of(PIN_SCL)) {
		wr(GPIO_IO(PIN_SDA), taken_io[0] & 2);
		wr(GPIO_IO(PIN_SCL), taken_io[1] & 2);
		wr(GPIO_CFG(PIN_SDA), taken_cfg[0]);
		wr(GPIO_CFG(PIN_SCL), taken_cfg[1]);
		printf("s6sy791: the I2C bus is not at rest (gpio%u reads %u, gpio%u reads %u): not touched\n", PIN_SDA,
		    level_of(PIN_SDA), PIN_SCL, level_of(PIN_SCL));
		return false;
	}
	return true;
}

static void
bus_give_back(void)
{
	wr(GPIO_IO(PIN_SDA), taken_io[0] & 2);
	wr(GPIO_IO(PIN_SCL), taken_io[1] & 2);
	wr(GPIO_CFG(PIN_SDA), taken_cfg[0]);
	wr(GPIO_CFG(PIN_SCL), taken_cfg[1]);
}

static uint8_t ts_address = 0x48;

/* The bytes a command answers with: the command written, then the same address again for the read. */
static bool
ts_read(uint8_t command, uint8_t *data, uint32_t length)
{
	bool answered;

	if (!bus_take()) {
		return false;
	}
	i2c_start();
	answered = i2c_write_byte((uint8_t)(ts_address << 1)) && i2c_write_byte(command);
	if (answered) {
		i2c_start();
		answered = i2c_write_byte((uint8_t)(ts_address << 1 | 1));
	}
	for (uint32_t i = 0; answered && i < length; i++) {
		data[i] = i2c_read_byte(i + 1 < length);
	}
	i2c_stop();
	bus_give_back();
	return answered;
}

static bool
ts_write(const uint8_t *data, uint32_t length)
{
	bool answered;

	if (!bus_take()) {
		return false;
	}
	i2c_start();
	answered = i2c_write_byte((uint8_t)(ts_address << 1));
	for (uint32_t i = 0; answered && i < length; i++) {
		answered = i2c_write_byte(data[i]);
	}
	i2c_stop();
	bus_give_back();
	return answered;
}

static bool
ts_write_byte(uint8_t command)
{
	return ts_write(&command, 1);
}

/* SMBus' write word: the command, then the value low byte first (the touch function is bit 0) */
static bool
ts_write_word(uint8_t command, uint16_t value)
{
	uint8_t data[3] = { command, (uint8_t)value, (uint8_t)(value >> 8) };

	return ts_write(data, sizeof(data));
}

static bool
ts_read_byte(uint8_t command, uint8_t *value)
{
	return ts_read(command, value, 1);
}

/* Does anything answer at all at this address: a read of one byte, whatever comes of it. */
static bool
i2c_present(uint8_t address)
{
	bool answered;

	if (!bus_take()) {
		return false;
	}
	i2c_start();
	answered = i2c_write_byte((uint8_t)(address << 1 | 1));
	if (answered) {
		(void)i2c_read_byte(false);
	}
	i2c_stop();
	bus_give_back();
	return answered;
}

/* ---- the chip ---------------------------------------------------------------------------------- */

#define TS_ADDRESS_SAMSUNG      0x48
#define TS_ADDRESS_SYNAPTICS    0x4b

/* commands (mainline Linux s6sy761.c: the same ones, the S6SY791 is of that family) */
#define CMD_SENSE_ON            0x10
#define CMD_SENSE_OFF           0x11
#define CMD_FIRMWARE_INTEGRITY  0x21
#define CMD_PANEL_INFO          0x23
#define CMD_TOUCH_FUNCTION      0x30            /* write word: bit 0 = touches */
#define CMD_DEVICE_ID           0x52
#define CMD_BOOT_STATUS         0x55
#define CMD_READ_ONE_EVENT      0x60
#define CMD_READ_ALL_EVENT      0x61
#define CMD_CLEAR_EVENT_STACK   0x62
#define CMD_APPLICATION_MODE    0xe4

#define BOOT_STATUS_APPLICATION 0x20
#define FIRMWARE_OK             0x80
#define TOUCH_FUNCTION_TOUCH    0x0001

#define MAX_FINGERS             10
#define MAX_EVENTS              32              /* what one report can carry, the header event included */
#define EVENT_SIZE              8

static uint32_t max_x = 1439, max_y = 3168;     /* the tree's; the chip's panel information overrides them */
static uint32_t flip;                           /* touch_flip: 1 mirrors left-right, 2 mirrors top-bottom */
static bool probe_only;                         /* touch_probe=1: look, do not take over */
static bool latency;                            /* touch_lat=1 */

static struct {
	uint32_t reports, touches, bad, last_x, last_y, reads;
} seen;

/* Which fingers were down in the last report, and where. */
static struct {
	bool down;
	uint32_t x, y;
} contact[MAX_FINGERS];

static uint8_t event_buffer[MAX_EVENTS * EVENT_SIZE];

static uint16_t
be16(const uint8_t *p)
{
	return (uint16_t)(p[0] << 8 | p[1]);
}

static void
ts_handle_events(const uint8_t *events, uint32_t count)
{
	static unsigned shown;

	for (uint32_t i = 0; i < count; i++) {
		const uint8_t *e = events + EVENT_SIZE * i;
		uint32_t event_id, finger, state, x, y, sx, sy;

		if (e[0] == 0) {
			return;                                 /* the end of the queue */
		}
		event_id = e[0] & 0x03;
		if (event_id != 0) {
			continue;                               /* 1: a status event, not a finger */
		}
		if ((e[0] & 0x3c) == 0) {
			continue;                               /* no finger in this one */
		}
		finger = ((e[0] & 0x3c) >> 2) - 1;
		if (finger >= MAX_FINGERS) {
			continue;
		}
		state = (e[0] & 0xc0) >> 6;
		x = (uint32_t)(e[1] << 4) | ((e[3] & 0xf0) >> 4);
		y = (uint32_t)(e[2] << 4) | (e[3] & 0x0f);

		if (state == 0) {
			continue;                               /* none: nothing to say */
		}
		if (state == 3) {                               /* release */
			if (contact[finger].down) {
				contact[finger].down = false;
				hakios_touch_post(HAKIOS_TOUCH_UP, finger, contact[finger].x, contact[finger].y);
			}
			continue;
		}

		seen.touches++;
		seen.last_x = x;
		seen.last_y = y;
		sx = x >= max_x ? 65535 : x * 65535 / max_x;
		sy = y >= max_y ? 65535 : y * 65535 / max_y;
		if (flip & 1) {
			sx = 65535 - sx;
		}
		if (flip & 2) {
			sy = 65535 - sy;
		}
		if (shown < 40) {
			shown++;
			printf("s6sy791: finger %u at %u, %u (of %u x %u): %s\n", finger, x, y, max_x, max_y,
			    state == 1 ? "down" : "moved");
		}
		if (latency) {
			uint64_t ns;

			absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
			printf("tl: t %u %u %u at %llu\n", finger, sx, sy, ns / 1000);
		}
		if (!contact[finger].down) {
			hakios_touch_post(HAKIOS_TOUCH_DOWN, finger, sx, sy);
		} else if (contact[finger].x != sx || contact[finger].y != sy) {
			hakios_touch_post(HAKIOS_TOUCH_MOVE, finger, sx, sy);
		} else {
			continue;
		}
		contact[finger].down = true;
		contact[finger].x = sx;
		contact[finger].y = sy;
	}
}

/* One look: the event at the head of the chip's queue, and everything queued behind it. */
static void
ts_read_events(void)
{
	uint32_t left;

	seen.reads++;
	if (!ts_read(CMD_READ_ONE_EVENT, event_buffer, EVENT_SIZE)) {
		if (seen.bad++ < 8) {
			printf("s6sy791: the event could not be read\n");
		}
		return;
	}
	if (event_buffer[0] == 0) {
		return;                                 /* nothing has happened */
	}
	left = event_buffer[7] & 0x3f;
	if (left > MAX_EVENTS - 1) {
		seen.bad++;
		return;
	}
	if (left != 0 && !ts_read(CMD_READ_ALL_EVENT, event_buffer + EVENT_SIZE, left * EVENT_SIZE)) {
		seen.bad++;
		if (seen.bad < 8) {
			printf("s6sy791: %u more events could not be read\n", left);
		}
		return;
	}
	seen.reports++;
	ts_handle_events(event_buffer, left + 1);
}

/* Is the chip there, awake, and in its application (not its boot loader)? */
static bool
ts_identify(void)
{
	uint8_t event[EVENT_SIZE] = { 0 }, status = 0, id[3] = { 0 }, info[11] = { 0 }, integrity = 0;
	unsigned kind;

	/* after a reset the chip posts an "information" event: the kind 2 or 7, the boot complete (byte 1 = 0) */
	for (int tries = 0; tries < 40; tries++) {
		if (ts_read(CMD_READ_ONE_EVENT, event, sizeof(event)) && event[0] != 0) {
			break;
		}
		sleep_ms(10);
	}
	kind = (event[0] >> 2) & 0xf;
	printf("s6sy791: first event %02x %02x %02x %02x %02x %02x %02x %02x\n", event[0], event[1], event[2],
	    event[3], event[4], event[5], event[6], event[7]);
	if (event[0] == 0) {
		printf("s6sy791: the chip answers but has posted nothing: not the identification event expected\n");
	} else if ((kind != 2 && kind != 7) || event[1] != 0) {
		printf("s6sy791: not an identification event (kind %u, byte 1 %u): reading on anyway\n", kind, event[1]);
	}

	if (!ts_read_byte(CMD_BOOT_STATUS, &status)) {
		printf("s6sy791: the boot status could not be read\n");
		return false;
	}
	printf("s6sy791: boot status %#04x (%s)\n", status, status == BOOT_STATUS_APPLICATION ? "application" :
	    status == 0x10 ? "BOOT LOADER: it has no firmware running" : "unknown");
	if (status != BOOT_STATUS_APPLICATION) {
		return false;
	}

	if (ts_read(CMD_DEVICE_ID, id, sizeof(id))) {
		printf("s6sy791: device id %#06x (%02x %02x %02x)\n", be16(id + 1), id[0], id[1], id[2]);
	} else {
		printf("s6sy791: the device id could not be read\n");
	}

	if (ts_read(CMD_PANEL_INFO, info, sizeof(info))) {
		uint32_t chips_x = be16(info), chips_y = be16(info + 2);

		printf("s6sy791: panel: %u x %u, %u transmit channels, %02x %02x %02x %02x %02x %02x %02x\n", chips_x,
		    chips_y, info[8], info[3], info[4], info[5], info[6], info[7], info[9]);
		if (chips_x > 1 && chips_y > 1) {
			max_x = chips_x;
			max_y = chips_y;
		}
	} else {
		printf("s6sy791: the panel information could not be read; keeping %u x %u from the device tree\n", max_x, max_y);
	}

	if (ts_read_byte(CMD_FIRMWARE_INTEGRITY, &integrity)) {
		printf("s6sy791: firmware integrity register %#04x (%s)\n", integrity, integrity == FIRMWARE_OK ?
		    "ok" : "not the 0x80 of the S6SY761's family: this chip's own answer, not a failure of the run");
	} else {
		printf("s6sy791: the firmware integrity could not be read\n");
	}
	return true;
}

/* Power the chip up from nothing, in the vendor driver's order: rails, a reset, and time to boot. */
static bool
power_on(void)
{
	uint32_t reset_ms = 10;

	PE_parse_boot_argn("touch_reset_ms", &reset_ms, sizeof(reset_ms));
	if (reset_ms > 1000) {
		reset_ms = 10;
	}
	reset_pin(false);
	int_pin_input();
	sleep_ms(reset_ms);

	if (!regulator_on("ldoa13", "ldo13", 3008)) {
		return false;
	}
	if (!regulator_on("ldoc1", "ldo1", 1800)) {
		return false;
	}
	sleep_ms(5);

	reset_pin(true);
	printf("s6sy791: reset high: cfg %#x io %#x\n", rd(GPIO_CFG(PIN_RESET)), rd(GPIO_IO(PIN_RESET)));
	sleep_ms(150);                          /* the chip boots its firmware: the information event comes then */
	return true;
}

/* ---- all of it together ------------------------------------------------------------------------ */

static void
scan_bus(void)
{
	unsigned found = 0;

	for (uint8_t address = 0x08; address <= 0x77; address++) {
		if (!i2c_present(address)) {
			continue;
		}
		found++;
		printf("s6sy791: the bus: %#04x answers%s%s\n", address,
		    address == TS_ADDRESS_SAMSUNG ? " (the S6SY791's address)" : "",
		    address == TS_ADDRESS_SYNAPTICS ? " (the Synaptics S3908's address)" : "");
	}
	printf("s6sy791: the bus: %u of 112 addresses answer\n", found);
}

static void
s6sy791_thread(__unused void *parameter, __unused wait_result_t result)
{
	uint32_t poll_us = 8000, idle_looks, value = 0;

	PE_parse_boot_argn("touch_poll_us", &poll_us, sizeof(poll_us));
	PE_parse_boot_argn("touch_i2c_us", &i2c_half_us, sizeof(i2c_half_us));
	PE_parse_boot_argn("touch_flip", &flip, sizeof(flip));
	if (PE_parse_boot_argn("touch_lat", &value, sizeof(value))) {
		latency = value != 0;
	}
	value = 0;
	if (PE_parse_boot_argn("touch_probe", &value, sizeof(value))) {
		probe_only = value != 0;
		pads_only = value == 2;
	}
	if (poll_us < 250 || poll_us > 100000) {
		poll_us = 8000;
	}
	if (i2c_half_us < 1 || i2c_half_us > 1000) {
		i2c_half_us = 5;
	}
	idle_looks = 80000 / poll_us;           /* a report read every 80 ms even with the line high, in case it is missed */

	tlmm = ml_io_map(TLMM_PHYS, TLMM_SIZE);
	rsc = ml_io_map(RSC_PHYS, RSC_SIZE);
	cmd_db = ml_io_map_wcomb(CMD_DB_PHYS, CMD_DB_SIZE);      /* memory, not registers: no alignment rules */
	if (tlmm == 0 || rsc == 0 || cmd_db == 0) {
		printf("s6sy791: the registers could not be mapped (tlmm %p, rsc %p, cmd-db %p)\n", (void *)tlmm,
		    (void *)rsc, (void *)cmd_db);
		thread_terminate(current_thread());
		return;
	}

	show_pins("as found");
	if (pads_only) {
		printf("s6sy791: touch_probe=2: the pads are read and the chip is left alone\n");
		thread_terminate(current_thread());
		return;
	}
	if (!power_on()) {
		printf("s6sy791: the supplies are not on; the chip is left alone\n");
		thread_terminate(current_thread());
		return;
	}
	show_pins("powered, out of reset");

	scan_bus();
	if (i2c_present(TS_ADDRESS_SAMSUNG)) {
		ts_address = TS_ADDRESS_SAMSUNG;
	} else if (i2c_present(TS_ADDRESS_SYNAPTICS)) {
		printf("s6sy791: nothing at %#04x; the Synaptics S3908 at %#04x answers (this driver does not drive it yet)\n",
		    TS_ADDRESS_SAMSUNG, TS_ADDRESS_SYNAPTICS);
		thread_terminate(current_thread());
		return;
	} else {
		printf("s6sy791: no touch controller answers on the bus: no touch screen\n");
		thread_terminate(current_thread());
		return;
	}

	if (!ts_identify()) {
		printf("s6sy791: the chip is not running its firmware; no touch screen\n");
		thread_terminate(current_thread());
		return;
	}

	if (probe_only) {
		printf("s6sy791: touch_probe=1: the chip is looked at and left alone\n");
		thread_terminate(current_thread());
		return;
	}

	/*
	 * The touch function on, then sensing: the order mainline Linux's s6sy761.c uses. The S6SY791
	 * does not acknowledge the touch-function write (the phone's chip answers every other command):
	 * said, and the run goes on, because it reports touches without it.
	 */
	if (!ts_write_word(CMD_TOUCH_FUNCTION, TOUCH_FUNCTION_TOUCH)) {
		printf("s6sy791: the touch function write was not acknowledged (the chip reports without it)\n");
	}
	if (!ts_write_byte(CMD_SENSE_ON)) {
		printf("s6sy791: sensing was not acknowledged; no touch screen\n");
		thread_terminate(current_thread());
		return;
	}
	(void)ts_write_byte(CMD_CLEAR_EVENT_STACK);
	printf("s6sy791: touch screen ready (%u x %u); the interrupt line is looked at every %u us\n", max_x, max_y, poll_us);

	unsigned idle = 0;

	for (unsigned tick = 1;; tick++) {
		if (int_low()) {
			ts_read_events();
		} else if (++idle >= idle_looks) {
			idle = 0;
			ts_read_events();
		}
		if (tick % (60 * (1000000 / poll_us)) == 0) {
			printf("s6sy791: so far %u reports in %u looks (%u bad), %u finger positions, the last at %u, %u; "
			    "interrupt line %s\n", seen.reports, seen.reads, seen.bad, seen.touches, seen.last_x,
			    seen.last_y, int_low() ? "low" : "high");
		}
		sleep_us(poll_us);
	}
}

extern "C" void
hakios_s6sy791_init(void)
{
	uint32_t enabled = 0;
	thread_t thread;

	if (!s6_lock) {
		s6_lock = lck_mtx_alloc_init(lck_grp_alloc_init("s6sy791", LCK_GRP_ATTR_NULL), LCK_ATTR_NULL);
	}

	if (!PE_parse_boot_argn("touchscreen", &enabled, sizeof(enabled)) || !enabled) {
		return;
	}
	if (kernel_thread_start(s6sy791_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
