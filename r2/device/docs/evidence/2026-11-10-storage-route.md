# Storage route: upstream ShengUfs with a kona table (2026-11-10)

## Decision

The port takes the **upstream** UFS component, catalog id `ShengUfs`
(runtime class `NativeUfs`, source `NativeUfs.cpp`, fetched at
`99d8bcd3e43dd24fc3c8437c67b5d4cc71ab94d0`, output
`build/sheng-ufs/libShengUfs.a`) instead of vendoring a board-local fork. The
SM8250 values live in the SoC repository and the component looks them up by
`compatible`.

`board.toml` selects `ShengUfs`, `VirtioMmioBlock` and `VerifiedRoot`, and
`[kernel] archives` names all three, because the archive plan reports
"needs component entries for" otherwise.

## Measured delta (SM8550 -> kona)

Taken from this unit's `stock_boot.dtb`, not from another port:

| Item | Value on this board |
| --- | --- |
| node | `ufshc@1d84000`, `compatible = "qcom,ufshc"` (generic, not sm8250-specific) |
| reg | `0x1d84000 + 0x3000` (`ufs_mem`), `0x1d90000 + 0x8000` (`ufs_ice`) |
| interrupt | SPI 265 (`0x109`), level high |
| PHY | `ufsphy_mem@1d87000`, two lanes per direction |
| clocks | `core_clk 0xa7`, `bus_aggr_clk 0x06`, `iface_clk 0xa6`, `core_clk_unipro 0xb3`, `core_clk_ice 0xaa`, `ref_clk` from RPMH `0x15:0`, `tx_lane0_sync 0xb2`, `rx_lane0_sync 0xb0`, `rx_lane1_sync 0xb1` |
| SMMU | the node carries **no** `iommus` property |

Against ShengUfs's own constants (`GccUfs.h`: `GCC_BASE 0x100000`,
`UFS_HC_BASE 0x1d84000`, `UFS_HC_SIZE 0x3000`) only these differ:

| Constant | ShengUfs (SM8550) | kona |
| --- | --- | --- |
| match string | `qcom,sm8550-ufshc` | `qcom,sm8250-ufshc` (our ADT node names it explicitly) |
| PHY window | `0x1d80000 + 0x2000` | `0x1d87000 + 0xe00` |
| GCC `axi` | `0x77018` | `0x77010` |
| GCC `ahb` | `0x77024` | `0x77018` |
| `tx_symbol_0` / `rx_symbol_0` | `0x77028` / `0x7702C` | `0x7701C` / `0x77020` |
| `unipro_core` | `0x77068` | `0x7705C` |
| `ice_core` | `0x77074` | `0x77064` |
| `phy_aux` | `0x770B0` | `0x7709C` |
| `rx_symbol_1` | `0x770CC` | `0x770B8` |
| `aggre_axi` | `0x770E4` | `0x770CC` |
| `core_reset` | GCC id from DT | id `0x21`, BCR at `0x77000` |
| SMMU | bypass on stream `0x60` | none |

The driver adopts the controller state the firmware left behind (`GccUfs.h`:
"No write is performed by NativeUfs yet", and the PHY and reset are untouched),
so the port is a constant substitution, not a new driver.

## Why the SoC repository owns the table

`AGENTS.md` puts shared SoC resources and operations in the selected SoC
repository, and the catalog scopes `ShengUfs` to "SM8550 UFS controller".
Table-driven lookup by `compatible` keeps one component for every kona board and
leaves the SM8250 numbers in `UniXNU-sm8250-common`.

## Blocker found while wiring it

The component build has no path to the SoC tree: `scripts/build-arm-driver.py`
adds fixed `-I` directories (`kits/...`, `drivers/...`, `build/...`) relative to
the assembled tree, and the tree has no `soc` entry
(`tree/{build,components.toml,drivers,kits,package,patches,refer,scripts}`).
Four changes make the design work:

1. `scripts/unixnu/tree.py`: link the resolved SoC feed into the tree as
   `soc` (next to the existing `package` and `components.toml` links).
2. `scripts/build-arm-driver.py`: add `-I <tree>/soc/include` for components
   that declare a SoC dependency.
3. `UniXNU-sm8250-common`: `include/ufs-soc-table.h` with the kona entry above.
4. `ShengUfs`: match `compatible` against the table (guarded by
   `__has_include`) and take the PHY/GCC/reset constants and the SMMU decision
   from the matched entry, falling back to the current SM8550 values.

The fetched copies currently sit in `out/feeds/ShengUfs-38c19c2b` and
`out/feeds/UniXNU-sm8250-common-671ba905`; a `local =` override in the feeds has
to pin them before the work is reproducible.
