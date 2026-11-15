# The community solution for watchdogd, and why it fits this board exactly

## The finding

Two advanced ports were cloned and read (GFander/UniXNU-kebab, mouse233/unixnu-fuxi). Both solve
the same wall this port is stuck on, and their explanation matches every observation here:

> The original AppleARMWatchdogTimer personality needs IOProviderClass = AppleARMIODevice and
> IONameMatch = wdt,s5l8960x. A board ADT without that device never registers a watchdog service;
> watchdogd then fails IOServiceOpen (EPERM under the original sandbox policy) and branches to its
> failure path with a breakpoint, so the process exits and the DEVELOPMENT kernel critical-process
> path panics. That is the watchdogd[..] exited -- exit reason namespace 2 subcode 0x5 line this
> port has been stopping at for two nights.

The fix is not a disabled plist and not a suppressed panic: it is to provide a real IOWatchdog
subclass service so the original watchdogd can open, enable and pet it.

## Why the fuxi port is directly usable here

mouse233/unixnu-fuxi builds its RamWatchdog against the original kernelcache
f18b4df865405f2a287b84977d7aca745e056d656d9fc9df1662970ce2969de7, and our
out/ipsw/21H461/kernelcache.decompressed has exactly that SHA256. Same OS build (21H461), same
IOWatchdog ABI (248-byte base object, 232-byte client, 171 vtable slots), same private offsets.
So the work is a port of their package, not a reimplementation.

## What was vendored

vendor-ram-watchdog/ now holds, byte for byte from that repository:

| file | role |
| --- | --- |
| RamWatchdog.cpp | the IOWatchdog subclass: real thread_call, attach to IOResources, calls the unmodified original base-class init, keeps the original registerService/kernel-monitoring/root/entitlement/exclusive-open/client dispatch; newUserClient delegates to the original |
| build-ram-watchdog.py | binds the five original symbols (base C2/D2, metaclass, vtable, init) from the kernelcache and archives the object into libRamRootProbe.a |
| ram_watchdog_original.py | parses the original module, slots and vtable from the kernelcache |
| check-ram-watchdog.py | the 30-item lifecycle/deadline/permission checks |
| README.md | their write-up (v46) |

## What the port needs next

1. build-ram-watchdog.py --reference <our kernel.macho> --kernelcache <21H461 kernelcache.decompressed> --output build/ram-watchdog. It reads the prelink plan from the reference .json sidecar (slide, extents, original_executable_sha256), which our XnuPrelink build already emits.
2. Add build-ram-watchdog.py to the kernel archives and the resulting archive to the link, with the boot pin they use (system_watchdog=true, ram-system-watchdog=1) so the service is published before PID 1 execs.
3. Rebuild and check the report line wdt=3 clients=.. polls=.. and that watchdogd no longer exits.

If the builder bindings do not match our kernel, the fallback is the kebab route: implement the
subclass and attach it to IOResources ourselves, then adapt the sandbox policy blob the way
adapt-watchdog-sandbox-policy.py does (same-length class name substitution, no MACF bypass).
