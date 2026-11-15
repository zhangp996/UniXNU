> **Stale snapshot.** `src/` below is the July source the catalog entry was drafted from. The
> component builds from `drivers/KonaUfs` (see `path` in `catalog-entry.toml`, and
> `unixnu/components.toml`), and the two copies have diverged since. Nothing reads `src/`; keep
> edits in `drivers/KonaUfs`.

# kona-ufs

A kona (SM8250) variant of [ShengUfs](https://github.com/UniXNU/ShengUfs), the driver the
component catalog lists with runtime class `NativeUfs`. This directory holds the plan and
the board inputs; the sources come from the upstream component with the constants below
substituted.

## Why a fork

`components.toml` scopes ShengUfs to "SM8550 UFS controller" with "SM8550 clocks and SMMU".
`porting.toml` records that selecting a component whose hardware scope is another SoC
programs the wrong registers, so the kona port needs its own table.

## What actually differs

ShengUfs **adopts** the firmware's controller state: `GccUfs.h` states "No write is
performed by NativeUfs yet", and the PHY and reset are likewise untouched. The port is
therefore a constant substitution plus dropping the SMMU step, not a new driver.

| Item | ShengUfs (SM8550) | kona (measured here) |
| --- | --- | --- |
| Host window | `0x01d84000` + `0x3000` | same |
| PHY window | `0x01d80000` + `0x2000` | **`0x1d87000` + `0xe00`** |
| GCC base | `0x00100000` | same |
| `axi` branch | `0x77018` | `0x77010` |
| `ahb` branch | `0x77024` | `0x77018` |
| `tx_symbol_0` | `0x77028` | `0x7701C` |
| `rx_symbol_0` | `0x7702C` | `0x77020` |
| `unipro_core` | `0x77068` | `0x7705C` |
| `ice_core` | `0x77074` | `0x77064` |
| `phy_aux` | `0x770B0` | `0x7709C` |
| `rx_symbol_1` | `0x770CC` | `0x770B8` |
| `aggre_axi` | `0x770E4` | `0x770CC` |
| `core_reset` | GCC id used in DT | GCC id `0x21`, BCR at `0x77000` |
| SMMU | bypass on stream `0x60` | **none**: the kona `ufshc` node has no `iommus` |
| ICE | present | `0x1d90000` + `0x8000`, unused by the root path |

## Status of the transport

The firmware probe `UfsProbeDxe` reproduced ShengUfs's command path on this controller
because the board had no driver yet: run/stop at `0x60`, `HCS` bit 1 wait, the same UTRD
words, `prd[3] = length - 1`, the `IS` acknowledge and a `dsb sy` before the ring. The queue
starts (`UTRLRSR 0x1`) and the controller reports `ocs 0`, but the response UPIU and the
data phase never land. `docs/bring-up.md` records the six builds that bracket this.

That result is the reason for moving the same code into the kernel: the reference driver
maps its buffers through `IOMemoryMap`, carries its own diagnostics per command and runs
with the kernel's cache and IOMMU setup, none of which the firmware path reproduces.

## Expected build integration

1. Copy the upstream sources, apply the table above in `GccUfs.h` and the PHY constant.
2. Drop the `smmuBypassStream` call (kona UFS is not behind the apps SMMU).
3. Add a component entry (id `KonaUfs`) and select it in `board.toml` with
   `CONFIG_XNU_NATIVE_UFS=y`.
4. Keep `UfsProbeDxe` in the firmware as the cold-state check.
