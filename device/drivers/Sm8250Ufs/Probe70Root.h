// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "RootTrace.h"

static SmUfsRootTraceSnapshot gProbe70Snapshots[3];
static uint32_t gProbe70Coherent[3];

static void probe70Capture(unsigned index)
{
	if (index < 3) gProbe70Coherent[index] = native_smufs_root_trace_snapshot(&gProbe70Snapshots[index]);
}

static bool probe70Valid()
{
	const SmUfsRootTraceSnapshot &s = gProbe70Snapshots[2];
	return gProbe70Coherent[0] && gProbe70Coherent[1] && gProbe70Coherent[2] &&
	       s.enabled && s.version == SMUFS_RT_CORE_VERSION && !s.partial && !s.lost &&
	       s.records[SMRT_BSD_ENTER].count && s.writes;
}

template <class Text> static void probe70Report(Text &text)
{
	const SmUfsRootTraceSnapshot &s = gProbe70Snapshots[2];
	const SmUfsRootTraceRecord *r = s.records;
	text.add("trace v%u en%u coherent%u/%u/%u partial%u lost%u writes%u\n", s.version, s.enabled,
	         gProbe70Coherent[0], gProbe70Coherent[1], gProbe70Coherent[2], s.partial, s.lost, s.writes);
	text.add("seq %u/%u/%u bsd%u/%u autoconf%u\n", gProbe70Snapshots[0].sequence,
	         gProbe70Snapshots[1].sequence, s.sequence, r[SMRT_BSD_ENTER].count, r[SMRT_BSD_RETURN].count,
	         r[SMRT_AUTOCONF_ENTER].count);
	text.add("kminit%u/%u pseudo%u iobsd%u/%u pub%u/%u\n", r[SMRT_KMINIT_ENTER].count,
	         r[SMRT_KMINIT_RETURN].count, r[SMRT_PSEUDO_DONE].count, r[SMRT_IOBSD_ENTER].count,
	         r[SMRT_IOBSD_RETURN].count, r[SMRT_IOBSD_PUBLISH_ENTER].count, r[SMRT_IOBSD_PUBLISH_RETURN].count);
	text.add("find%u/%u attempt%u resource%u/%u present%d\n", r[SMRT_FIND_ENTER].count,
	         r[SMRT_FIND_RETURN].count, r[SMRT_FIND_ENTER].arg, r[SMRT_RESOURCE_ENTER].count,
	         r[SMRT_RESOURCE_RETURN].count, r[SMRT_RESOURCE_RETURN].result);
	text.add("match%u inputs%x props%x quiet%u/%u wait%u/%u present%d\n", r[SMRT_MATCH_SOURCE].arg,
	         r[SMRT_INPUTS].arg, r[SMRT_BSD_PROPERTIES].arg, r[SMRT_QUIET_ENTER].count,
	         r[SMRT_QUIET_RETURN].count, r[SMRT_ROOT_WAIT_ENTER].count, r[SMRT_ROOT_WAIT_RETURN].count,
	         r[SMRT_ROOT_WAIT_RETURN].result);
	const SmUfsRootTraceRecord &selected = r[SMRT_SELECTED];
	text.add("select count%u valid%u dev%x major%u minor%u flags%x\n", selected.count, selected.dev_valid,
	         selected.dev, selected.dev >> 24, selected.dev & 0xffffffu, unsigned(selected.result));
	text.add("find-ret k%u r%d caller%u k%u r%d fallback%u\n", r[SMRT_FIND_RETURN].result_kind,
	         r[SMRT_FIND_RETURN].result, r[SMRT_FIND_CALL_RETURN].count,
	         r[SMRT_FIND_CALL_RETURN].result_kind, r[SMRT_FIND_CALL_RETURN].result, r[SMRT_FALLBACK].count);
	text.add("setconf%u/%u rootmedia%u/%u resolved%u:%d\n", r[SMRT_SETCONF_ENTER].count,
	         r[SMRT_SETCONF_RETURN].count, r[SMRT_ROOTMEDIA_ENTER].count, r[SMRT_ROOTMEDIA_RETURN].count,
	         r[SMRT_MEDIA_RESOLVED].count, r[SMRT_MEDIA_RESOLVED].result);
	text.add("mount%u/%u k%u errno%d secure%u/%u\n", r[SMRT_MOUNT_ENTER].count, r[SMRT_MOUNT_RETURN].count,
	         r[SMRT_MOUNT_RETURN].result_kind, r[SMRT_MOUNT_RETURN].result,
	         r[SMRT_SECURE_ENTER].count, r[SMRT_SECURE_RETURN].count);
	text.add("vfsroot%u/%u errno%d published%u valid%u dev%x\n", r[SMRT_VFSROOT_ENTER].count,
	         r[SMRT_VFSROOT_RETURN].count, r[SMRT_VFSROOT_RETURN].result, r[SMRT_ROOT_PUBLISHED].count,
	         r[SMRT_ROOT_PUBLISHED].dev_valid, r[SMRT_ROOT_PUBLISHED].dev);
	text.add("auth%u/%u errno%d devfs%u/%u errno%d\n", r[SMRT_AUTH_ENTER].count,
	         r[SMRT_AUTH_RETURN].count, r[SMRT_AUTH_RETURN].result, r[SMRT_DEVFS_ENTER].count,
	         r[SMRT_DEVFS_RETURN].count, r[SMRT_DEVFS_RETURN].result);
}
