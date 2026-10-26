# Xiaomi 15 (dada)

dada is the Xiaomi 15 with the SM8750 SoC. The port below is an independent community project. The UniXNU organization does not maintain it.

| Repository | Maintainer | State |
| --- | --- | --- |
| [UniXNU-dada](https://github.com/7Night-kqx/UniXNU-dada) | [7Night-kqx](https://github.com/7Night-kqx) | UEFI firmware works; UniXNU and XNU not run on hardware |

## State

| Stage | State |
| --- | --- |
| UEFI firmware, framebuffer, menu, UFS, side buttons, UART7 | works in Mu-Silicium |
| UFS controller bring-up | the UIC path is primed, Hibernate is left with a UIC command and the MCQ switch at 0x300 is read; the first storage read is next |
| UniXNU menu and configuration | not run |
| Payload volume | board values written; device inputs missing |
| DarwinBoot handoff | not run |
| XNU kernel entry and platform expert | first goal; the SM8750 platform expert does not exist |
| UFS root and userland | later |

Nothing has been booted on hardware. `board.toml` marks device values that still need a measurement with `TODO(device)`.

## Maintenance

The maintainer owns the repository and its release decisions. UniXNU documentation lists the port and its state; it does not take over maintenance.
