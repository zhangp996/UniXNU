# Nothing Phone (2) (pong)

pong is the Nothing Phone (2) with the SM8475 SoC. The port below is an independent community project. The UniXNU organization does not maintain it. The target is iOS 16.6.1 (20G81) with the iPad7,1 / J120 identity.

| Repository | Maintainer | State |
| --- | --- | --- |
| [UniXNU-pong](https://github.com/Chengdong-Yu/UniXNU-pong) | [Chengdong-Yu](https://github.com/Chengdong-Yu) | iOS 16.6.1 home screen; display and touch work |
| [uefi-pong](https://github.com/Chengdong-Yu/uefi-pong) | [Chengdong-Yu](https://github.com/Chengdong-Yu) | Mu-Silicium firmware with `pongPkg` |

## State

| Area | State |
| --- | --- |
| iOS 16.6.1 boot | reaches the home screen |
| Display | system UI visible on the 1080x2412 panel |
| Touch and buttons | the Pong touch path works; power and volume keys work |
| UFS and APFS | the native UFS and APFS boot path is connected; some mount paths are still in progress |
| Writable Data | a local userdata root-image builder is provided; the image is generated on the host |
| Adreno 730 and Metal | driver and Metal sources are included; hardware acceleration is not confirmed |
| Wi-Fi, Bluetooth, camera, audio, sensors | not implemented |

The device repository fetches only the needed iOS 16.6.1 components from the restore image by HTTP range. Apple restore files, generated userdata images and GPU firmware stay outside the repository.

## Maintenance

The maintainer owns the repositories and their release decisions. UniXNU documentation lists the port and its state; it does not take over maintenance.
