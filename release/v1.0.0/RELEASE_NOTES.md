# HINK E-Paper Community v1.0.0

这是面向 HINK 2.13 英寸黑白红电子价签爱好者的首个完整社区版固件。它对应设备内部版本 `19`，包含 BLE 无线传图、低功耗运行路径和 OTA v2 M2 双槽更新能力。

固件下载与完整操作说明：<https://hapcoderwei.github.io/hink-epaper/firmware.html>

## 适用硬件

- 屏幕 FPC：`HINK-E0213A162-FPC-A0`
- MCU：`TLSR8359F512ET32`
- HINK Board ID：`0x213A`

其他 TLSR8359 价签不能只凭芯片型号判断兼容；GPIO、屏幕协议和 Flash 布局必须一致。

## 版本身份

- GitHub 版本：`v1.0.0`
- 设备内部版本：`19`
- 文件：`HINK_E0213A162_Community_v1.0.0.bin`
- 长度：61,320 字节（`0xEF88`）
- SHA-256：`126b37fc516e76f103131e0595a36112ff828ded8f372021adb562d3f30bbe97`
- HINK 清单：format 1、flags 0、board `0x213A`、firmware version 19
- Payload CRC32：`78F78A8F`
- Manifest CRC32：`E005D149`
- Telink CRC32：`68894C14`

## 主要功能

- 通过 Web Bluetooth 发送图片，浏览器端完成 `250×122` 横向预览和黑白红三色处理。
- 价签广播名为 `HINK_XXXXXX`，后缀来自持久化 BLE MAC，便于区分多块设备。
- 屏幕刷新完成后进入深睡，MCU 使用正常低功耗路径。
- OTA v2 M2 使用 A/B 双启动槽、候选镜像完整校验、两步安装确认、追加式恢复日志、5 秒试运行确认和受限自动回滚。

## 验证结果

- [x] HINK 清单、板型、长度、payload CRC32、清单 CRC32 和 Telink 原生 CRC 验证通过。
- [x] 完整主机回归测试通过。
- [x] 最终真机从故障候选 B/v20 自动恢复到 A/v19；恢复日志 75 条有效、0 条损坏。
- [x] A/v19 的 BLE 重连、设备版本、活动槽和三色刷新确认通过。
- [x] 双槽、首扇区备份和恢复日志经过 SWire 读回核对。
- [ ] v19 没有单独重新测量待机电流。低功耗代码路径沿用正常构建；30～40 µA 是 M0 阶段实测基线，不作为 v19 的保证值。

## 首次烧录

原厂固件、B1、v0.1.x 或版本未知的价签必须先通过 TLSRPGM/SWire 烧录：取下电池，`SWM → P6-3 SWS`、`GND → P6-1 GND`、唯一的 `3.0～3.3 V → P6-5 VCC`。先读芯片信息并备份完整 512 KiB Flash，再从 `0x000000` 写入本固件并读回 61,320 字节比较。

禁止 5 V，禁止电池与外部电源并接，禁止未备份就写入，禁止使用 `-c / CPU Stall`。

## 后续 OTA

只有已经运行 OTA v2 M2、网页能读出版本和槽位的价签才可通过项目 Pages 更新。上传达到 100% 不等于安装完成；必须等待板端完整校验通过，再依次完成“准备安装”和“安装并重启”，最后重连核对版本、槽位和三色刷新。

旧 ATC OTA 页面及其 `Send Firmware` / `final flash` 始终禁止使用。

## 已知限制

- OTA v2 M2 没有固件签名或身份认证，只能在可信的近距离 BLE 环境使用。
- 没有独立不可变 bootloader；候选若损坏到无法执行早期恢复代码，仍可能需要 SWire 救援。
- 只对上述已确认板型负责，不承诺兼容外观相似或仅 MCU 相同的价签。
