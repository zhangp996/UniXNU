# Agent instructions

Maintain the baseband IOKit bridge, QMUX/QRTR protocols and IPA data path. Board power, modem boot and firmware packaging belong in the device or SoC repository.

Read README.md, platform/coachz/README.md and the UniXNU porting/baseband guide before changes. Preserve runtime class names, protocol numbers, imported author history and license notices. Update UPSTREAM.json when synchronizing imported sources. Keep firmware and EFS/NV data outside Git.

Use the selected kernel's compile records. Run the affected existing protocol models and build the archive. Update the README and UniXNU English/Chinese pages together. Keep hardware observations separate from host checks.
