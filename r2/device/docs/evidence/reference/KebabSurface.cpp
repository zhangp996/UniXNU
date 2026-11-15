/*
 * Built-in IOSurface for KEBAB.
 *
 * Apple's IOSurface kext is closed. IOSurface.framework (iOS 16.7) finds
 * IOServiceNameMatching("IOCoreSurfaceRoot"), opens it with type 0 and
 * talks to the user client below:
 *
 *   sel 13  limits: struct out 0x18, u16 at +0x16 must be 1 (ABI version),
 *           +0 offset alignment mask, +4 bytes-per-row alignment mask
 *   sel 0   create: scalar [parent id], struct in = serialized property
 *           dictionary (binary or XML), struct out = lock result (0xa68)
 *   sel 4   lookup by id: scalar [id], struct out = lock result
 *   sel 34  lookup by mach port: scalar [port name], struct out = lock result
 *   sel 35  create mach port: scalars [id, options], scalar out [port name]
 *   sel 9   set value: scalars [id] (or an 8-byte {id, pad} header) and a
 *           serialized [value, key] pair or dictionary to merge; out = the
 *           new generation
 *   sel 10  update non-bulk value cache: out = {u32, generation,
 *           serialized non-bulk dictionary}
 *   sel 11  remove value: [id] and a NUL-terminated key (empty = all)
 *   sel 27  set bulk attachments (struct 0x88, 0x1b): copy the changed
 *           fields into the shared state page at +0x30
 *   traps   0/1 use count +/- (id, category), 2 lock / 3 unlock
 *           (id, options, &seed), 4/5 release (id)
 *
 * The lock result tells the library where the surface lives in the
 * caller's address space: +0 pixels, +8 shared state page (registry id,
 * seed at +0xc, values generation at +0x20 and its serialized size at
 * +0x28, bulk attachments from +0x30), +0x10 timestamps, then id, sizes
 * and geometry (layout in kebab_surface_fill_result). Every surface is
 * wired kernel memory mapped into each task that creates or looks it up;
 * mappings are counted per client and dropped on release or close.
 *
 * Selectors this kernel does not implement return kIOReturnUnsupported
 * instead of a zeroed success: a client that believes an unimplemented
 * call worked reads garbage.
 */
#include "KebabSurface.h"

#if defined(KEBAB)
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/system.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSSerialize.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSUnserialize.h>
#include <sys/proc.h>

enum {
	kKebabSurfaceCreate             = 0,
	kKebabSurfaceLookup             = 4,
	kKebabSurfaceSetValue           = 9,
	kKebabSurfaceUpdateValueCache   = 10,
	kKebabSurfaceRemoveValue        = 11,
	kKebabSurfaceLimits             = 13,
	kKebabSurfaceSetBulkAttachments = 27,   /* 0x1b */
	kKebabSurfaceLookupFromPort     = 34,   /* 0x22 */
	kKebabSurfaceCreatePort         = 35,   /* 0x23 */
};

enum {
	kKebabSurfaceTrapIncrementUse   = 0,
	kKebabSurfaceTrapDecrementUse   = 1,
	kKebabSurfaceTrapLock           = 2,
	kKebabSurfaceTrapUnlock         = 3,
	kKebabSurfaceTrapRelease        = 4,
	kKebabSurfaceTrapReleaseFailed  = 5,
};

#define KEBAB_SURFACE_RESULT_SIZE       0xa68
#define KEBAB_SURFACE_MAX_ID            4096
#define KEBAB_SURFACE_MAX_DIM           16384
#define KEBAB_SURFACE_MAX_ALLOC         (256u << 20)
#define KEBAB_SURFACE_MAX_PROPS         (64u << 10)
#define KEBAB_SURFACE_ROW_ALIGN         64
#define KEBAB_SURFACE_TIMESTAMPS        0x800   /* offset in the state page */
#define KEBAB_SURFACE_STATE_SEED        0xc     /* u32 in the state page */
#define KEBAB_SURFACE_STATE_GENERATION  0x20    /* u32, values generation */
#define KEBAB_SURFACE_STATE_VALUE_SIZE  0x28    /* u32, serialized values size */
#define KEBAB_SURFACE_STATE_BULK        0x30    /* bulk attachment fields */
#define KEBAB_SURFACE_VALUES_MAX        (64u << 10)
#define kIOSurfaceLockReadOnly          0x1

static IOLock       *kebab_surface_lock;
static KebabSurface *kebab_surface_table[KEBAB_SURFACE_MAX_ID];
static uint32_t      kebab_surface_users[KEBAB_SURFACE_MAX_ID];
static uint32_t      kebab_surface_next_id = 1;
static uint32_t      kebab_surface_creates_logged;
static uint64_t      kebab_surface_wired;       /* bytes of buffer surfaces */
#define KEBAB_SURFACE_WIRED_MAX         (1536ull << 20)

OSDefineMetaClassAndStructors(KebabSurface, OSObject);

void
KebabSurface::free()
{
	if (OSDynamicCast(IOBufferMemoryDescriptor, memory) != NULL) {
		OSAddAtomic64(-(SInt64)allocSize, (volatile SInt64 *)&kebab_surface_wired);
	}
	OSSafeReleaseNULL(kernelMap);
	OSSafeReleaseNULL(values);
	OSSafeReleaseNULL(memory);
	OSSafeReleaseNULL(state);
	OSObject::free();
}

uintptr_t
KebabSurface::kernelAddress()
{
	IOBufferMemoryDescriptor *buffer = OSDynamicCast(IOBufferMemoryDescriptor, memory);
	if (buffer != NULL) {
		return (uintptr_t)buffer->getBytesNoCopy();
	}
	IOLockLock(kebab_surface_lock);
	if (kernelMap == NULL && memory != NULL) {
		kernelMap = memory->map(kIOMapWriteCombineCache);
	}
	IOLockUnlock(kebab_surface_lock);
	return kernelMap != NULL ? (uintptr_t)kernelMap->getVirtualAddress() : 0;
}

static uint32_t *
kebab_surface_seed(KebabSurface *s)
{
	/* The state page is page-aligned, so the seed is naturally aligned; the
	 * integer cast keeps the compiler from warning about the byte pointer. */
	return (uint32_t *)(uintptr_t)((uint8_t *)s->state->getBytesNoCopy() +
	    KEBAB_SURFACE_STATE_SEED);
}

/*
 * Give the surface an id and a table slot; the table holds one retain and a
 * user count (creators, client mappings, kernel holders). Lock held.
 */
static bool
kebab_surface_insert_locked(KebabSurface *s)
{
	for (uint32_t n = 0; n < KEBAB_SURFACE_MAX_ID - 1; n++) {
		uint32_t id = kebab_surface_next_id;
		kebab_surface_next_id = id + 1 < KEBAB_SURFACE_MAX_ID ? id + 1 : 1;
		if (kebab_surface_table[id] == NULL) {
			s->id = id;
			s->retain();
			kebab_surface_table[id] = s;
			kebab_surface_users[id] = 1;
			*(uint64_t *)s->state->getBytesNoCopy() = id;   /* registry id */
			return true;
		}
	}
	return false;
}

/* Add a user to a surface that may have left the table (port lookups). */
static bool
kebab_surface_get(KebabSurface *s)
{
	bool ok = true;
	IOLockLock(kebab_surface_lock);
	if (kebab_surface_table[s->id] == s) {
		kebab_surface_users[s->id]++;
	} else if (kebab_surface_table[s->id] == NULL) {
		s->retain();
		kebab_surface_table[s->id] = s;
		kebab_surface_users[s->id] = 1;
	} else {
		ok = false;
	}
	IOLockUnlock(kebab_surface_lock);
	return ok;
}

/* Drop a user; the last one removes the surface from the table. */
static void
kebab_surface_put(KebabSurface *s)
{
	bool drop = false;
	IOLockLock(kebab_surface_lock);
	if (kebab_surface_table[s->id] == s && --kebab_surface_users[s->id] == 0) {
		kebab_surface_table[s->id] = NULL;
		drop = true;
	}
	IOLockUnlock(kebab_surface_lock);
	if (drop) {
		s->release();
	}
}

KebabSurface *
kebab_surface_copy(uint32_t id)
{
	KebabSurface *s = NULL;
	if (id == 0 || id >= KEBAB_SURFACE_MAX_ID || kebab_surface_lock == NULL) {
		return NULL;
	}
	IOLockLock(kebab_surface_lock);
	s = kebab_surface_table[id];
	if (s != NULL) {
		s->retain();
	}
	IOLockUnlock(kebab_surface_lock);
	return s;
}

/* The table lock; the framebuffer may publish a surface before the root starts. */
static void
kebab_surface_init_lock(void)
{
	if (kebab_surface_lock == NULL) {
		IOLock *lock = IOLockAlloc();
		if (!OSCompareAndSwapPtr(NULL, lock, &kebab_surface_lock)) {
			IOLockFree(lock);
		}
	}
}

static KebabSurface *
kebab_surface_alloc(void)
{
	kebab_surface_init_lock();
	KebabSurface *s = OSTypeAlloc(KebabSurface);
	if (s == NULL || !s->init()) {
		OSSafeReleaseNULL(s);
		return NULL;
	}
	s->state = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
	    kIODirectionInOut | kIOMemoryKernelUserShared, page_size, page_size);
	if (s->state == NULL) {
		s->release();
		return NULL;
	}
	bzero(s->state->getBytesNoCopy(), page_size);
	s->values = NULL;
	s->generation = 0;
	s->valueSize = 0;
	return s;
}

static bool
kebab_surface_publish(KebabSurface *s)
{
	bool ok;
	IOLockLock(kebab_surface_lock);
	ok = kebab_surface_insert_locked(s);
	IOLockUnlock(kebab_surface_lock);
	return ok;
}

KebabSurface *
kebab_surface_create_physical(uint64_t phys, uint32_t width, uint32_t height,
    uint32_t bytesPerRow, uint32_t pixelFormat)
{
	KebabSurface *s = kebab_surface_alloc();
	if (s == NULL) {
		return NULL;
	}
	s->width = width;
	s->height = height;
	s->bytesPerRow = bytesPerRow;
	s->bytesPerElement = 4;
	s->elementWidth = s->elementHeight = 1;
	s->pixelFormat = pixelFormat;
	s->allocSize = (uint32_t)round_page(bytesPerRow * height);
	s->memory = IOMemoryDescriptor::withPhysicalAddress(phys, s->allocSize, kIODirectionInOut);
	s->mapCache = kIOMapWriteCombineCache;
	if (s->memory == NULL || !kebab_surface_publish(s)) {
		s->release();
		return NULL;
	}
	return s;
}

mach_port_name_t
kebab_surface_port_in_task(KebabSurface *s, task_t task)
{
	mach_port_name_t name = 0;
	IOUserClient::copyPortNameForObjectInTask(task, s, &name);
	return name;
}

static uint32_t
kebab_dict_u32(OSDictionary *dict, const char *key, uint32_t def)
{
	OSNumber *n = OSDynamicCast(OSNumber, dict->getObject(key));
	return n != NULL ? n->unsigned32BitValue() : def;
}

static uint32_t
kebab_div_up(uint32_t a, uint32_t b)
{
	return b > 1 ? (a + b - 1) / b : a;
}

static uint32_t
kebab_row_bytes(uint32_t width, uint32_t ew, uint32_t bpe)
{
	uint64_t row = (uint64_t)kebab_div_up(width, ew) * bpe;
	return (uint32_t)((row + KEBAB_SURFACE_ROW_ALIGN - 1) & ~(uint64_t)(KEBAB_SURFACE_ROW_ALIGN - 1));
}

/* Geometry from the creation properties (IOSurface.h keys). */
static IOReturn
kebab_surface_layout(KebabSurface *s, OSDictionary *props)
{
	uint64_t end;

	s->width = kebab_dict_u32(props, "IOSurfaceWidth", 0);
	s->height = kebab_dict_u32(props, "IOSurfaceHeight", 0);
	s->bytesPerElement = (uint16_t)kebab_dict_u32(props, "IOSurfaceBytesPerElement", 4);
	s->elementWidth = (uint8_t)kebab_dict_u32(props, "IOSurfaceElementWidth", 1);
	s->elementHeight = (uint8_t)kebab_dict_u32(props, "IOSurfaceElementHeight", 1);
	s->pixelFormat = kebab_dict_u32(props, "IOSurfacePixelFormat", 0);
	s->cacheMode = kebab_dict_u32(props, "IOSurfaceCacheMode", 0);
	s->offset = kebab_dict_u32(props, "IOSurfaceOffset", 0);
	if (s->width > KEBAB_SURFACE_MAX_DIM || s->height > KEBAB_SURFACE_MAX_DIM ||
	    s->bytesPerElement > 64 || s->elementWidth == 0 || s->elementHeight == 0) {
		return kIOReturnBadArgument;
	}
	s->bytesPerRow = kebab_dict_u32(props, "IOSurfaceBytesPerRow",
	    kebab_row_bytes(s->width, s->elementWidth, s->bytesPerElement));
	if (s->bytesPerRow < (uint64_t)kebab_div_up(s->width, s->elementWidth) * s->bytesPerElement) {
		return kIOReturnBadArgument;
	}
	end = (uint64_t)s->offset + (uint64_t)s->bytesPerRow * kebab_div_up(s->height, s->elementHeight);

	OSArray *planes = OSDynamicCast(OSArray, props->getObject("IOSurfacePlaneInfo"));
	s->planeCount = 0;
	if (planes != NULL) {
		uint64_t next = s->offset;
		if (planes->getCount() > KEBAB_SURFACE_MAX_PLANES) {
			return kIOReturnBadArgument;
		}
		for (unsigned int i = 0; i < planes->getCount(); i++) {
			OSDictionary *pd = OSDynamicCast(OSDictionary, planes->getObject(i));
			struct kebab_surface_plane *p = &s->planes[i];
			if (pd == NULL) {
				return kIOReturnBadArgument;
			}
			p->width = kebab_dict_u32(pd, "IOSurfacePlaneWidth", s->width);
			p->height = kebab_dict_u32(pd, "IOSurfacePlaneHeight", s->height);
			p->bytesPerElement = (uint16_t)kebab_dict_u32(pd, "IOSurfacePlaneBytesPerElement", 1);
			p->elementWidth = (uint8_t)kebab_dict_u32(pd, "IOSurfacePlaneElementWidth", 1);
			p->elementHeight = (uint8_t)kebab_dict_u32(pd, "IOSurfacePlaneElementHeight", 1);
			if (p->width > KEBAB_SURFACE_MAX_DIM || p->height > KEBAB_SURFACE_MAX_DIM ||
			    p->bytesPerElement > 64 || p->elementWidth == 0 || p->elementHeight == 0) {
				return kIOReturnBadArgument;
			}
			p->bytesPerRow = kebab_dict_u32(pd, "IOSurfacePlaneBytesPerRow",
			    kebab_row_bytes(p->width, p->elementWidth, p->bytesPerElement));
			p->offset = kebab_dict_u32(pd, "IOSurfacePlaneOffset", (uint32_t)next);
			p->size = kebab_dict_u32(pd, "IOSurfacePlaneSize",
			    p->bytesPerRow * kebab_div_up(p->height, p->elementHeight));
			next = round_page((uint64_t)p->offset + p->size);
			if ((uint64_t)p->offset + p->size > end) {
				end = (uint64_t)p->offset + p->size;
			}
		}
		s->planeCount = planes->getCount();
	}

	uint64_t alloc = kebab_dict_u32(props, "IOSurfaceAllocSize", 0);
	if (alloc < end) {
		alloc = end;
	}
	alloc = round_page(alloc);
	if (alloc == 0 || alloc > KEBAB_SURFACE_MAX_ALLOC) {
		return kIOReturnNoMemory;
	}
	s->allocSize = (uint32_t)alloc;
	return kIOReturnSuccess;
}

/* Per-client mapping of one surface. */
class KebabSurfaceRef : public OSObject {
	OSDeclareDefaultStructors(KebabSurfaceRef);

public:
	KebabSurface *surface;
	IOMemoryMap  *data;
	IOMemoryMap  *state;
	uint32_t      count;

	bool init() APPLE_KEXT_OVERRIDE { return OSObject::init(); }
	void free() APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(KebabSurfaceRef, OSObject);

void
KebabSurfaceRef::free()
{
	OSSafeReleaseNULL(data);
	OSSafeReleaseNULL(state);
	if (surface != NULL) {
		kebab_surface_put(surface);
		surface->release();
		surface = NULL;
	}
	OSObject::free();
}

class IOCoreSurfaceRoot : public IOService {
	OSDeclareDefaultStructors(IOCoreSurfaceRoot);

public:
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(IOCoreSurfaceRoot, IOService);

bool
IOCoreSurfaceRoot::start(IOService *provider)
{
	if (!IOService::start(provider)) {
		return false;
	}
	kebab_surface_init_lock();
	setProperty(kIOUserClientClassKey, "IOSurfaceRootUserClient");
	registerService();
	IOLog("KEBAB SURF: IOCoreSurfaceRoot registered\n");
	return true;
}

class IOSurfaceRootUserClient : public IOUserClient {
	OSDeclareDefaultStructors(IOSurfaceRootUserClient);

	task_t   fTask;
	IOLock  *fLock;
	OSArray *fRefs;

	KebabSurfaceRef *findRef(uint32_t id, unsigned int *index);
	IOReturn addRef(KebabSurface *s, KebabSurfaceRef **refOut, bool *existing);
	IOReturn returnSurface(KebabSurface *s, IOExternalMethodArguments *args);
	IOReturn create(IOExternalMethodArguments *args);

public:
	bool initWithTask(task_t owningTask, void *securityID, UInt32 type) APPLE_KEXT_OVERRIDE;
	void free() APPLE_KEXT_OVERRIDE;
	IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
	    IOExternalMethodDispatch *dispatch, OSObject *target,
	    void *reference) APPLE_KEXT_OVERRIDE;
	IOExternalTrap *getTargetAndTrapForIndex(IOService **targetP, UInt32 index) APPLE_KEXT_OVERRIDE;

	IOReturn trapUseCount(void *id, void *category, void *, void *, void *, void *);
	IOReturn trapLock(void *id, void *options, void *seedOut, void *, void *, void *);
	IOReturn trapUnlock(void *id, void *options, void *seedOut, void *, void *, void *);
	IOReturn trapRelease(void *id, void *, void *, void *, void *, void *);
};

OSDefineMetaClassAndStructors(IOSurfaceRootUserClient, IOUserClient);

static uint64_t kebab_surf_logged;      /* selectors / traps already logged (< 64) */

static void
kebab_surf_log_once(const char *what, uint32_t n)
{
	uint32_t bit = (what[0] == 't' ? 48 : 0) + n;
	if (bit >= 64 || (kebab_surf_logged & (1ULL << bit))) {
		return;
	}
	kebab_surf_logged |= 1ULL << bit;
	char name[32];
	proc_selfname(name, sizeof(name));
	IOLog("KEBAB SURF: %s %s %u\n", name, what, n);
}

bool
IOSurfaceRootUserClient::initWithTask(task_t owningTask, void *securityID, UInt32 type)
{
	if (!IOUserClient::initWithTask(owningTask, securityID, type)) {
		return false;
	}
	fTask = owningTask;
	fLock = IOLockAlloc();
	fRefs = OSArray::withCapacity(16);
	return fLock != NULL && fRefs != NULL;
}

void
IOSurfaceRootUserClient::free()
{
	OSSafeReleaseNULL(fRefs);
	if (fLock != NULL) {
		IOLockFree(fLock);
		fLock = NULL;
	}
	IOUserClient::free();
}

IOReturn
IOSurfaceRootUserClient::clientClose(void)
{
	IOLockLock(fLock);
	fRefs->flushCollection();
	IOLockUnlock(fLock);
	terminate();
	return kIOReturnSuccess;
}

/* Lock held. */
KebabSurfaceRef *
IOSurfaceRootUserClient::findRef(uint32_t id, unsigned int *index)
{
	for (unsigned int i = 0; i < fRefs->getCount(); i++) {
		KebabSurfaceRef *ref = (KebabSurfaceRef *)fRefs->getObject(i);
		if (ref->surface->id == id) {
			if (index != NULL) {
				*index = i;
			}
			return ref;
		}
	}
	return NULL;
}

/*
 * Count one more create/lookup of s by this client, mapping it into the
 * task the first time. Takes over the caller's user count on s.
 */
IOReturn
IOSurfaceRootUserClient::addRef(KebabSurface *s, KebabSurfaceRef **refOut, bool *existing)
{
	*existing = false;
	IOLockLock(fLock);
	KebabSurfaceRef *ref = findRef(s->id, NULL);
	if (ref != NULL && ref->surface == s) {
		IOLockUnlock(fLock);
		kebab_surface_put(s);
		*refOut = ref;
		*existing = true;
		return kIOReturnSuccess;
	}
	IOLockUnlock(fLock);

	ref = OSTypeAlloc(KebabSurfaceRef);
	if (ref == NULL || !ref->init()) {
		OSSafeReleaseNULL(ref);
		kebab_surface_put(s);
		return kIOReturnNoMemory;
	}
	s->retain();
	ref->surface = s;
	ref->count = 1;
	ref->data = s->memory->createMappingInTask(fTask, 0, kIOMapAnywhere | s->mapCache);
	ref->state = s->state->createMappingInTask(fTask, 0, kIOMapAnywhere | kIOMapReadOnly);
	if (ref->data == NULL || ref->state == NULL) {
		ref->release();
		return kIOReturnVMError;
	}
	IOLockLock(fLock);
	KebabSurfaceRef *raced = findRef(s->id, NULL);
	if (raced != NULL && raced->surface == s) {
		/* Another thread mapped it meanwhile: count there, drop ours. */
		raced->count++;
		IOLockUnlock(fLock);
		ref->release();
		*refOut = raced;
		return kIOReturnSuccess;
	}
	fRefs->setObject(ref);
	IOLockUnlock(fLock);
	ref->release();         /* fRefs holds it */
	*refOut = ref;
	return kIOReturnSuccess;
}

static void
put32(uint8_t *p, uint32_t off, uint32_t v)
{
	memcpy(p + off, &v, 4);
}

static void
put64(uint8_t *p, uint32_t off, uint64_t v)
{
	memcpy(p + off, &v, 8);
}

static uint64_t
get64(const uint8_t *p, uint32_t off)
{
	uint64_t v;
	memcpy(&v, p + off, 8);
	return v;
}

static uint32_t
get32(const uint8_t *p, uint32_t off)
{
	uint32_t v;
	memcpy(&v, p + off, 4);
	return v;
}

static KebabSurface *kebab_surface_held(IOLock *lock, OSArray *refs, uint32_t id);

/*
 * Copy the caller's structure input into kernel memory. Both the inband
 * pointer and the out-of-line descriptor form are accepted.
 */
static IOReturn
kebab_structure_in(IOExternalMethodArguments *args, uint8_t **buffer, uint32_t *size, uint32_t max)
{
	uint32_t len;
	uint8_t *copy;

	if (args->structureInputDescriptor != NULL) {
		len = (uint32_t)args->structureInputDescriptor->getLength();
		if (len == 0 || len > max) {
			return kIOReturnBadArgument;
		}
		copy = (uint8_t *)IOMallocData(len);
		if (copy == NULL) {
			return kIOReturnNoMemory;
		}
		if (args->structureInputDescriptor->readBytes(0, copy, len) != len) {
			IOFreeData(copy, len);
			return kIOReturnBadArgument;
		}
	} else {
		len = args->structureInputSize;
		if (len == 0 || len > max || args->structureInput == NULL) {
			return kIOReturnBadArgument;
		}
		copy = (uint8_t *)IOMallocData(len);
		if (copy == NULL) {
			return kIOReturnNoMemory;
		}
		memcpy(copy, args->structureInput, len);
	}
	*buffer = copy;
	*size = len;
	return kIOReturnSuccess;
}

/*
 * Publish a new values generation in the shared state page: odd, never
 * zero, and two ahead of the previous one, which is what the client checks.
 * Called with kebab_surface_lock held.
 */
static void
kebab_surface_values_changed(KebabSurface *s, uint32_t serializedSize)
{
	uint8_t *state = (uint8_t *)s->state->getBytesNoCopy();

	s->generation = (s->generation + 2) | 1;
	s->valueSize = serializedSize;
	put32(state, KEBAB_SURFACE_STATE_GENERATION, s->generation);
	put32(state, KEBAB_SURFACE_STATE_VALUE_SIZE, s->valueSize);
}

/* Serialize the values dictionary as the binary property list the client
 * expects. Called with kebab_surface_lock held; the caller frees the copy. */
static IOReturn
kebab_surface_values_serialize(KebabSurface *s, uint8_t **buffer, uint32_t *size)
{
	OSSerialize *serialize;
	uint8_t *copy;
	uint32_t len;

	if (s->values == NULL) {
		s->values = OSDictionary::withCapacity(8);
		if (s->values == NULL) {
			return kIOReturnNoMemory;
		}
	}
	serialize = OSSerialize::binaryWithCapacity(256);
	if (serialize == NULL) {
		return kIOReturnNoMemory;
	}
	if (!s->values->serialize(serialize)) {
		serialize->release();
		return kIOReturnNoMemory;
	}
	len = serialize->getLength();
	if (len == 0 || len > KEBAB_SURFACE_VALUES_MAX) {
		serialize->release();
		return kIOReturnNoMemory;
	}
	copy = (uint8_t *)IOMallocData(len);
	if (copy == NULL) {
		serialize->release();
		return kIOReturnNoMemory;
	}
	memcpy(copy, serialize->text(), len);
	serialize->release();
	*buffer = copy;
	*size = len;
	return kIOReturnSuccess;
}

/*
 * Apply one selector-9 payload: an OS-serialized [value, key] pair sets one
 * key, a serialized dictionary merges. Returns the new generation.
 */
static IOReturn
kebab_surface_values_apply(KebabSurface *s, const uint8_t *payload, uint32_t len,
    uint32_t *generationOut)
{
	OSObject *object = OSUnserializeXML((const char *)payload, len);
	OSArray *array;
	OSDictionary *dict;
	OSSerialize *serialize;
	bool ok = false;
	uint32_t serialized = 0;

	if (object == NULL) {
		return kIOReturnBadArgument;
	}
	IOLockLock(kebab_surface_lock);
	if (s->values == NULL) {
		s->values = OSDictionary::withCapacity(8);
	}
	dict = OSDynamicCast(OSDictionary, object);
	array = OSDynamicCast(OSArray, object);
	if (s->values == NULL) {
		/* no memory */
	} else if (dict != NULL) {
		ok = s->values->merge(dict);
	} else if (array != NULL && array->getCount() == 2) {
		OSString *key = OSDynamicCast(OSString, array->getObject(1));
		OSObject *value = array->getObject(0);
		if (key != NULL && value != NULL) {
			ok = s->values->setObject(key, value);
		}
	} else if (array != NULL && array->getCount() == 1) {
		OSDictionary *merge = OSDynamicCast(OSDictionary, array->getObject(0));
		if (merge != NULL) {
			ok = s->values->merge(merge);
		}
	}
	if (ok) {
		serialize = OSSerialize::binaryWithCapacity(256);
		if (serialize != NULL) {
			if (s->values->serialize(serialize)) {
				serialized = serialize->getLength();
			}
			serialize->release();
		}
		kebab_surface_values_changed(s, serialized);
		*generationOut = s->generation;
	}
	IOLockUnlock(kebab_surface_lock);
	object->release();
	return ok ? kIOReturnSuccess : kIOReturnBadArgument;
}

/* Remove one key, or everything when the key is empty. */
static IOReturn
kebab_surface_values_remove(KebabSurface *s, const char *key, uint32_t *generationOut)
{
	OSSerialize *serialize;
	uint32_t serialized = 0;

	IOLockLock(kebab_surface_lock);
	if (s->values == NULL) {
		s->values = OSDictionary::withCapacity(8);
	}
	if (s->values == NULL) {
		IOLockUnlock(kebab_surface_lock);
		return kIOReturnNoMemory;
	}
	if (key != NULL && key[0] != '\0') {
		s->values->removeObject(key);
	} else {
		s->values->flushCollection();
	}
	serialize = OSSerialize::binaryWithCapacity(256);
	if (serialize != NULL) {
		if (s->values->serialize(serialize)) {
			serialized = serialize->getLength();
		}
		serialize->release();
	}
	kebab_surface_values_changed(s, serialized);
	*generationOut = s->generation;
	IOLockUnlock(kebab_surface_lock);
	return kIOReturnSuccess;
}

/* The selector-0x1b attachment fields and where they live in the state page. */
struct kebab_bulk_field {
	uint64_t mask;
	uint32_t source;
	uint32_t destination;
	uint32_t length;
};

static const struct kebab_bulk_field kebab_bulk_fields[] = {
	{ 1ULL << 0,  0x00, KEBAB_SURFACE_STATE_BULK + 0x00, 32 },
	{ 1ULL << 1,  0x20, KEBAB_SURFACE_STATE_BULK + 0x20, 16 },
	{ 1ULL << 2,  0x30, KEBAB_SURFACE_STATE_BULK + 0x30, 8 },
	{ 1ULL << 3,  0x38, KEBAB_SURFACE_STATE_BULK + 0x38, 1 },
	{ 1ULL << 4,  0x39, KEBAB_SURFACE_STATE_BULK + 0x39, 1 },
	{ 1ULL << 5,  0x3a, KEBAB_SURFACE_STATE_BULK + 0x3a, 1 },
	{ 1ULL << 6,  0x3b, KEBAB_SURFACE_STATE_BULK + 0x3b, 1 },
	{ 1ULL << 7,  0x3c, KEBAB_SURFACE_STATE_BULK + 0x3c, 1 },
	{ 1ULL << 8,  0x3d, KEBAB_SURFACE_STATE_BULK + 0x3d, 1 },
	{ 1ULL << 9,  0x3e, KEBAB_SURFACE_STATE_BULK + 0x3e, 1 },
	{ 1ULL << 10, 0x3f, KEBAB_SURFACE_STATE_BULK + 0x3f, 1 },
	{ 1ULL << 11, 0x40, KEBAB_SURFACE_STATE_BULK + 0x40, 24 },
	{ 1ULL << 12, 0x58, KEBAB_SURFACE_STATE_BULK + 0x58, 4 },
	{ 1ULL << 13, 0x5c, KEBAB_SURFACE_STATE_BULK + 0x5c, 8 },
	{ 1ULL << 14, 0x64, KEBAB_SURFACE_STATE_BULK + 0x64, 1 },
};

/*
 * Copy the fields the caller marked changed into the shared state page. The
 * client and the kernel both read the attachments from there, so this is the
 * real implementation and not a placeholder. "Valid size" bounds which of
 * the 0x70 struct bytes may be read.
 */
static IOReturn
kebab_surface_bulk(KebabSurface *s, const uint8_t *in, uint32_t len)
{
	uint8_t *state;
	uint64_t valid;
	uint64_t changed;

	if (len < 0x88) {
		return kIOReturnBadArgument;
	}
	valid = get64(in, 0x70);
	changed = get64(in, 0x78);
	if (valid > 0x70) {
		valid = 0x70;
	}
	state = (uint8_t *)s->state->getBytesNoCopy();
	IOLockLock(kebab_surface_lock);
	for (unsigned int i = 0; i < sizeof(kebab_bulk_fields) / sizeof(kebab_bulk_fields[0]); i++) {
		const struct kebab_bulk_field *field = &kebab_bulk_fields[i];
		if (!(changed & field->mask)) {
			continue;
		}
		if ((uint64_t)field->source + field->length > valid) {
			continue;       /* the caller did not mark these bytes valid */
		}
		memcpy(state + field->destination, in + field->source, field->length);
	}
	IOLockUnlock(kebab_surface_lock);
	return kIOReturnSuccess;
}

/* Selectors 9 and 11 answer with the new generation, as a scalar or as a
 * four-byte structure output depending on how the client called. */
static void
kebab_generation_out(IOExternalMethodArguments *args, uint32_t generation)
{
	if (args->scalarOutputCount >= 1) {
		args->scalarOutput[0] = generation;
	}
	if (args->structureOutput != NULL && args->structureOutputSize >= 4) {
		memcpy(args->structureOutput, &generation, 4);
	}
}

/* IOSurface.framework's lock result (copied to IOSurfaceClient + 0x68). */
static void
kebab_surface_fill_result(KebabSurface *s, KebabSurfaceRef *ref, uint8_t *out)
{
	bzero(out, KEBAB_SURFACE_RESULT_SIZE);
	put64(out, 0x00, ref->data->getAddress());
	put64(out, 0x08, ref->state->getAddress());
	put64(out, 0x10, ref->state->getAddress() + KEBAB_SURFACE_TIMESTAMPS);
	put32(out, 0x18, s->id);
	put32(out, 0x1c, s->allocSize);
	put32(out, 0x20, s->width);
	put32(out, 0x24, s->height);
	put32(out, 0x28, s->bytesPerRow);
	put32(out, 0x2c, s->offset);
	put32(out, 0x30, s->pixelFormat);
	put32(out, 0x34, s->allocSize);
	put32(out, 0x38, s->planeCount);
	memcpy(out + 0x40, &s->bytesPerElement, 2);
	out[0x42] = s->elementWidth;
	out[0x43] = s->elementHeight;
	put32(out, 0x44, s->cacheMode);
	put32(out, 0x50, s->parentID);
	put32(out, 0x54, 1);
	for (uint32_t i = 0; i < s->planeCount; i++) {
		uint8_t *p = out + 0x68 + i * 0x80;
		const struct kebab_surface_plane *pl = &s->planes[i];
		put32(p, 0x00, pl->width);
		put32(p, 0x04, pl->height);
		put32(p, 0x08, pl->bytesPerRow);
		put32(p, 0x0c, pl->offset);
		put32(p, 0x14, pl->size);
		memcpy(p + 0x18, &pl->bytesPerElement, 2);
		p[0x1a] = pl->elementWidth;
		p[0x1b] = pl->elementHeight;
	}
}

static IOReturn
kebab_struct_out(IOExternalMethodArguments *args, const void *buf, uint32_t len)
{
	if (args->structureOutputDescriptor != NULL) {
		if (len > args->structureOutputDescriptor->getLength()) {
			return kIOReturnNoSpace;
		}
		args->structureOutputDescriptor->writeBytes(0, buf, len);
		args->structureOutputDescriptorSize = len;
		return kIOReturnSuccess;
	}
	if (args->structureOutput == NULL || len > args->structureOutputSize) {
		return kIOReturnNoSpace;
	}
	memcpy(args->structureOutput, buf, len);
	args->structureOutputSize = len;
	return kIOReturnSuccess;
}

/* Map s for this client and answer with its lock result. Takes s's user count. */
IOReturn
IOSurfaceRootUserClient::returnSurface(KebabSurface *s, IOExternalMethodArguments *args)
{
	KebabSurfaceRef *ref = NULL;
	bool existing = false;
	IOReturn ret = addRef(s, &ref, &existing);
	if (ret != kIOReturnSuccess) {
		return ret;
	}
	uint8_t *out = (uint8_t *)IOMallocZeroData(KEBAB_SURFACE_RESULT_SIZE);
	if (out == NULL) {
		ret = kIOReturnNoMemory;
	} else {
		kebab_surface_fill_result(s, ref, out);
		if (existing) {
			put64(out, 0x00, ~0ULL);        /* already yours: reuse the cached client */
		}
		ret = kebab_struct_out(args, out, KEBAB_SURFACE_RESULT_SIZE);
		IOFreeData(out, KEBAB_SURFACE_RESULT_SIZE);
	}
	if (ret != kIOReturnSuccess && !existing) {
		/* The caller never learns the id: undo this create/lookup. */
		trapRelease((void *)(uintptr_t)s->id, NULL, NULL, NULL, NULL, NULL);
	}
	return ret;
}

IOReturn
IOSurfaceRootUserClient::create(IOExternalMethodArguments *args)
{
	const char *buf = (const char *)args->structureInput;
	uint32_t len = args->structureInputSize;
	char *copy = NULL;
	IOReturn ret;

	if (args->structureInputDescriptor != NULL) {
		len = (uint32_t)args->structureInputDescriptor->getLength();
		if (len == 0 || len > KEBAB_SURFACE_MAX_PROPS) {
			return kIOReturnBadArgument;
		}
		copy = (char *)IOMallocData(len);
		if (copy == NULL) {
			return kIOReturnNoMemory;
		}
		args->structureInputDescriptor->readBytes(0, copy, len);
		buf = copy;
	}
	OSObject *obj = (buf != NULL && len > 0) ? OSUnserializeXML(buf, len) : NULL;
	if (copy != NULL) {
		IOFreeData(copy, len);
	}
	OSDictionary *props = OSDynamicCast(OSDictionary, obj);
	if (props == NULL) {
		OSSafeReleaseNULL(obj);
		IOLog("KEBAB SURF: create: bad property dictionary (%u bytes)\n", len);
		return kIOReturnBadArgument;
	}

	KebabSurface *s = kebab_surface_alloc();
	if (s == NULL) {
		props->release();
		return kIOReturnNoMemory;
	}
	s->parentID = args->scalarInputCount > 0 ? (uint32_t)args->scalarInput[0] : 0;
	ret = kebab_surface_layout(s, props);
	if (ret == kIOReturnSuccess) {
		IOBufferMemoryDescriptor *mem = NULL;
		if ((uint64_t)OSAddAtomic64(s->allocSize, (volatile SInt64 *)&kebab_surface_wired) +
		    s->allocSize <= KEBAB_SURFACE_WIRED_MAX) {
			mem = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
			    kIODirectionInOut | kIOMemoryKernelUserShared, s->allocSize, page_size);
		}
		if (mem == NULL) {
			OSAddAtomic64(-(SInt64)s->allocSize, (volatile SInt64 *)&kebab_surface_wired);
			ret = kIOReturnNoMemory;
		} else {
			bzero(mem->getBytesNoCopy(), s->allocSize);
			s->memory = mem;
			s->mapCache = kIOMapDefaultCache;
			if (!kebab_surface_publish(s)) {
				ret = kIOReturnNoResources;
			}
		}
	}
	if (ret != kIOReturnSuccess || kebab_surface_creates_logged < 64) {
		char name[32];
		proc_selfname(name, sizeof(name));
		kebab_surface_creates_logged++;
		IOLog("KEBAB SURF: %s create id %u %ux%u fmt 0x%x bpe %u bpr %u planes %u size %u keys %u -> 0x%x\n",
		    name, s->id, s->width, s->height, s->pixelFormat, s->bytesPerElement,
		    s->bytesPerRow, s->planeCount, s->allocSize, props->getCount(), ret);
	}
	props->release();
	if (ret != kIOReturnSuccess) {
		s->release();
		return ret;
	}
	ret = returnSurface(s, args);   /* takes the creator's user count */
	s->release();
	return ret;
}

IOReturn
IOSurfaceRootUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
    __unused IOExternalMethodDispatch *dispatch, __unused OSObject *target,
    __unused void *reference)
{
	uint64_t arg0 = args->scalarInputCount > 0 ? args->scalarInput[0] : 0;

	switch (selector) {
	case kKebabSurfaceLimits: {
		uint8_t limits[0x18] = { 0 };
		put32(limits, 0x0, KEBAB_SURFACE_ROW_ALIGN - 1);
		put32(limits, 0x4, KEBAB_SURFACE_ROW_ALIGN - 1);
		limits[0x16] = 1;
		kebab_surf_log_once("sel", selector);
		return kebab_struct_out(args, limits, sizeof(limits));
	}
	case kKebabSurfaceCreate:
		return create(args);
	case kKebabSurfaceLookup: {
		KebabSurface *s = kebab_surface_copy((uint32_t)arg0);
		IOReturn ret = kIOReturnNotFound;
		if (s != NULL) {
			ret = kebab_surface_get(s) ? returnSurface(s, args) : kIOReturnNotFound;
			s->release();
		}
		kebab_surf_log_once("sel", selector);
		return ret;
	}
	case kKebabSurfaceLookupFromPort: {
		OSObject *obj = NULL;
		IOReturn ret = IOUserClient::copyObjectForPortNameInTask(fTask, (mach_port_name_t)arg0, &obj);
		KebabSurface *s = OSDynamicCast(KebabSurface, obj);
		if (ret == kIOReturnSuccess) {
			ret = (s != NULL && kebab_surface_get(s)) ? returnSurface(s, args) : kIOReturnBadArgument;
		}
		OSSafeReleaseNULL(obj);
		kebab_surf_log_once("sel", selector);
		return ret;
	}
	case kKebabSurfaceCreatePort: {
		KebabSurface *s = kebab_surface_copy((uint32_t)arg0);
		if (s == NULL || args->scalarOutputCount < 1) {
			OSSafeReleaseNULL(s);
			return kIOReturnNotFound;
		}
		args->scalarOutput[0] = kebab_surface_port_in_task(s, fTask);
		args->scalarOutputCount = 1;
		s->release();
		kebab_surf_log_once("sel", selector);
		return kIOReturnSuccess;
	}
	case kKebabSurfaceSetValue: {
		uint8_t *buffer = NULL;
		uint8_t *payload;
		uint32_t len = 0;
		uint32_t id = (uint32_t)arg0;
		uint32_t generation = 0;
		KebabSurface *s;
		IOReturn ret;

		ret = kebab_structure_in(args, &buffer, &len, KEBAB_SURFACE_VALUES_MAX);
		if (ret != kIOReturnSuccess) {
			kebab_surf_log_once("sel", selector);
			return ret;
		}
		payload = buffer;
		if (args->scalarInputCount == 0 && len >= 8) {
			id = get32(payload, 0);
			payload += 8;
			len -= 8;
		}
		s = kebab_surface_held(fLock, fRefs, id);
		if (s == NULL) {
			ret = kIOReturnNotFound;
		} else {
			ret = kebab_surface_values_apply(s, payload, len, &generation);
			if (ret == kIOReturnBadArgument && len > 8) {
				/* Some builds keep the {id, pad} header in the struct
				 * even when the id is also a scalar; try past it. */
				ret = kebab_surface_values_apply(s, payload + 8, len - 8, &generation);
			}
			if (ret == kIOReturnSuccess) {
				kebab_generation_out(args, generation);
			}
			s->release();
		}
		uint32_t total = len + (uint32_t)(payload - buffer);
		IOFreeData(buffer, total);
		kebab_surf_log_once("sel", selector);
		return ret;
	}
	case kKebabSurfaceUpdateValueCache: {
		uint8_t *blob = NULL;
		uint8_t *out;
		uint32_t blobLen = 0;
		uint32_t generation = 0;
		uint32_t needed;
		uint32_t id = (uint32_t)arg0;
		KebabSurface *s;
		IOReturn ret;

		if (args->scalarInputCount == 0 && args->structureInput != NULL &&
		    args->structureInputSize >= 4) {
			id = get32((const uint8_t *)args->structureInput, 0);
		}
		s = kebab_surface_held(fLock, fRefs, id);
		if (s == NULL) {
			kebab_surf_log_once("sel", selector);
			return kIOReturnNotFound;
		}
		IOLockLock(kebab_surface_lock);
		ret = kebab_surface_values_serialize(s, &blob, &blobLen);
		generation = s->generation;
		IOLockUnlock(kebab_surface_lock);
		s->release();
		if (ret != kIOReturnSuccess) {
			kebab_surf_log_once("sel", selector);
			return ret;
		}
		needed = 8 + blobLen;
		out = (uint8_t *)IOMallocData(needed);
		if (out == NULL) {
			IOFreeData(blob, blobLen);
			kebab_surf_log_once("sel", selector);
			return kIOReturnNoMemory;
		}
		put32(out, 0x0, 0);
		put32(out, 0x4, generation);
		memcpy(out + 8, blob, blobLen);
		IOFreeData(blob, blobLen);

		if (args->structureOutputDescriptor != NULL) {
			IOByteCount size = args->structureOutputDescriptor->getLength();
			if (size < needed) {
				args->structureOutputDescriptorSize = needed;
				ret = kIOReturnNoSpace;
			} else if (args->structureOutputDescriptor->writeBytes(0, out, needed) != needed) {
				ret = kIOReturnVMError;
			} else {
				args->structureOutputDescriptorSize = needed;
				ret = kIOReturnSuccess;
			}
		} else if (args->structureOutput == NULL || args->structureOutputSize < needed) {
			/* Tell the client how much it needs; it retries. The inband
			 * reply buffer is 4 KiB, so only advertise a size that fits. */
			if (needed <= 4096) {
				args->structureOutputSize = needed;
			}
			if (args->scalarOutputCount >= 1) {
				args->scalarOutput[0] = needed;
			}
			ret = kIOReturnNoSpace;
		} else {
			memcpy(args->structureOutput, out, needed);
			if (args->structureOutputSize > needed) {
				bzero((uint8_t *)args->structureOutput + needed,
				    args->structureOutputSize - needed);
			}
			ret = kIOReturnSuccess;
		}
		IOFreeData(out, needed);
		kebab_surf_log_once("sel", selector);
		return ret;
	}
	case kKebabSurfaceRemoveValue: {
		uint8_t *buffer = NULL;
		uint8_t *payload;
		uint32_t len = 0;
		uint32_t id = (uint32_t)arg0;
		uint32_t generation = 0;
		char key[256];
		KebabSurface *s;
		IOReturn ret;

		if (args->structureInputDescriptor != NULL ||
		    (args->structureInput != NULL && args->structureInputSize > 0)) {
			ret = kebab_structure_in(args, &buffer, &len, sizeof(key));
			if (ret != kIOReturnSuccess) {
				kebab_surf_log_once("sel", selector);
				return ret;
			}
		}
		payload = buffer;
		if (buffer != NULL && args->scalarInputCount == 0 && len >= 8) {
			id = get32(payload, 0);
			payload += 8;
			len -= 8;
		}
		uint32_t i = 0;
		while (buffer != NULL && i < len && i < sizeof(key) - 1 && payload[i] != '\0') {
			key[i] = payload[i];
			i++;
		}
		key[i] = '\0';
		s = kebab_surface_held(fLock, fRefs, id);
		if (s == NULL) {
			ret = kIOReturnNotFound;
		} else {
			ret = kebab_surface_values_remove(s, key, &generation);
			if (ret == kIOReturnSuccess) {
				kebab_generation_out(args, generation);
			}
			s->release();
		}
		if (buffer != NULL) {
			uint32_t total = len + (uint32_t)(payload - buffer);
			IOFreeData(buffer, total);
		}
		kebab_surf_log_once("sel", selector);
		return ret;
	}
	case kKebabSurfaceSetBulkAttachments: {
		uint8_t *buffer = NULL;
		uint32_t len = 0;
		KebabSurface *s;
		IOReturn ret;

		ret = kebab_structure_in(args, &buffer, &len, 0x100);
		if (ret != kIOReturnSuccess) {
			kebab_surf_log_once("sel", selector);
			return ret;
		}
		if (len < 0x88) {
			ret = kIOReturnBadArgument;
		} else {
			s = kebab_surface_held(fLock, fRefs, get32(buffer, 0x80));
			if (s == NULL) {
				ret = kIOReturnNotFound;
			} else {
				ret = kebab_surface_bulk(s, buffer, len);
				s->release();
			}
		}
		IOFreeData(buffer, len);
		kebab_surf_log_once("sel", selector);
		return ret;
	}
	default:
		/* Unimplemented: a zeroed success makes the client believe it got an
		 * answer it never got. */
		for (uint32_t i = 0; i < args->scalarOutputCount; i++) {
			args->scalarOutput[i] = 0;
		}
		if (args->structureOutput != NULL) {
			bzero(args->structureOutput, args->structureOutputSize);
		}
		kebab_surf_log_once("sel", selector);
		return kIOReturnUnsupported;
	}
}

IOReturn
IOSurfaceRootUserClient::trapUseCount(__unused void *id, __unused void *category,
    void *, void *, void *, void *)
{
	return kIOReturnSuccess;
}

static IOReturn
kebab_surface_seed_out(KebabSurface *s, void *seedOut, bool bump)
{
	uint32_t seed = bump ? OSIncrementAtomic((volatile SInt32 *)kebab_surface_seed(s)) + 1 :
	    *kebab_surface_seed(s);
	if (seedOut != NULL && copyout(&seed, (user_addr_t)seedOut, sizeof(seed)) != 0) {
		return kIOReturnBadArgument;
	}
	return kIOReturnSuccess;
}

/* Retained surface this client created or looked up, or NULL. */
static KebabSurface *
kebab_surface_held(IOLock *lock, OSArray *refs, uint32_t id)
{
	KebabSurface *s = NULL;
	IOLockLock(lock);
	for (unsigned int i = 0; i < refs->getCount(); i++) {
		KebabSurfaceRef *ref = (KebabSurfaceRef *)refs->getObject(i);
		if (ref->surface->id == id) {
			s = ref->surface;
			s->retain();
			break;
		}
	}
	IOLockUnlock(lock);
	return s;
}

IOReturn
IOSurfaceRootUserClient::trapLock(void *id, __unused void *options, void *seedOut,
    void *, void *, void *)
{
	KebabSurface *s = kebab_surface_held(fLock, fRefs, (uint32_t)(uintptr_t)id);
	if (s == NULL) {
		return kIOReturnNotFound;
	}
	IOReturn ret = kebab_surface_seed_out(s, seedOut, false);
	s->release();
	return ret;
}

IOReturn
IOSurfaceRootUserClient::trapUnlock(void *id, void *options, void *seedOut,
    void *, void *, void *)
{
	KebabSurface *s = kebab_surface_held(fLock, fRefs, (uint32_t)(uintptr_t)id);
	if (s == NULL) {
		return kIOReturnNotFound;
	}
	IOReturn ret = kebab_surface_seed_out(s, seedOut,
	    !((uintptr_t)options & kIOSurfaceLockReadOnly));
	s->release();
	return ret;
}

IOReturn
IOSurfaceRootUserClient::trapRelease(void *id, void *, void *, void *, void *, void *)
{
	unsigned int index;
	IOLockLock(fLock);
	KebabSurfaceRef *ref = findRef((uint32_t)(uintptr_t)id, &index);
	if (ref != NULL && --ref->count == 0) {
		fRefs->removeObject(index);     /* unmaps and drops the user count */
	}
	IOLockUnlock(fLock);
	return ref != NULL ? kIOReturnSuccess : kIOReturnNotFound;
}

static IOExternalTrap kebab_surface_traps[] = {
	[kKebabSurfaceTrapIncrementUse]  = { NULL, (IOTrap) & IOSurfaceRootUserClient::trapUseCount },
	[kKebabSurfaceTrapDecrementUse]  = { NULL, (IOTrap) & IOSurfaceRootUserClient::trapUseCount },
	[kKebabSurfaceTrapLock]          = { NULL, (IOTrap) & IOSurfaceRootUserClient::trapLock },
	[kKebabSurfaceTrapUnlock]        = { NULL, (IOTrap) & IOSurfaceRootUserClient::trapUnlock },
	[kKebabSurfaceTrapRelease]       = { NULL, (IOTrap) & IOSurfaceRootUserClient::trapRelease },
	[kKebabSurfaceTrapReleaseFailed] = { NULL, (IOTrap) & IOSurfaceRootUserClient::trapRelease },
};

IOExternalTrap *
IOSurfaceRootUserClient::getTargetAndTrapForIndex(IOService **targetP, UInt32 index)
{
	if (index >= sizeof(kebab_surface_traps) / sizeof(kebab_surface_traps[0])) {
		kebab_surf_log_once("trap", index);
		return NULL;
	}
	kebab_surf_log_once("trap", index);
	*targetP = this;
	return &kebab_surface_traps[index];
}
#endif /* KEBAB */
