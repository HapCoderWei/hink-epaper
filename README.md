# HINK 2.13 黑白红电子价签改造

TLSR8359F512ET32 + HINK-E0213A162-FPC-A0：BLE 无线传图、三色墨水屏驱动、低功耗 B1 阶段归档。

**在线工具：[打开 HINK 网页传图](https://hapcoderwei.github.io/hink-epaper/)**

使用 Mac Chrome 打开 HTTPS 页面，开启蓝牙，点击连接 HINK，选择图片并发送。网页是纯前端，图片在浏览器内处理，通过本机蓝牙直达附近价签；不需要业务后端。完整刷新后关闭页面断开连接，避免持续连接耗电。详见 [网页使用说明](docs/WEB.md)。

## 当前里程碑：v0.1.0-b1

- 真机传图、黑白红全屏刷新正常，固件通过 SWire 烧录并逐字节回读匹配。
- 3.0 V 供电、20 mA 档：未连接待机约 **42 µA**，连接无传图约 **765 µA**，刷新后关闭网页 60 秒回到约 **42 µA**。
- MCU 为普通 suspend，约 1 秒广播，**没有启用 deep retention**；三色 LED 保持关闭。
- 两颗 CR2450 并联；假设每颗 600 mAh、42 µA 为真实平均值，纯待机理论约 3.26 年。不是剩余电量测量或寿命保证。
- 仪表精度/脉冲响应未知，42 µA 是用户实测表上读数，不是高精度积分平均值。

这是已经可用的阶段性基准，后续优化另开版本，不把 SDK 配置等同于实测睡眠证明。

名称修正版 **v0.1.1-name** 将广播响应和标准 GATT 名称统一为 `HINK_XXXXXX`，使用价签持久化 BLE MAC 的低三字节作为六位后缀。同一价签重启后不变，便于同时烧录和管理多块价签；显示与功耗策略仍为 B1。预编译文件见 [v0.1.1-name Release](https://github.com/HapCoderWei/hink-epaper/releases/tag/v0.1.1-name)。

## 文档与目录

- [B1 里程碑与待办](docs/MILESTONE_B1.md)
- [硬件映射、屏幕协议与移植](docs/HARDWARE.md)
- [编译、测试与烧录](docs/BUILD.md)
- `firmware/src/`：当前 B1 应用源码；`firmware/makefile`：构建参数。
- `tests/`：使用实际显示/GPIO代码的主机桩测试。
- `site/`：独立网页工具，GitHub Pages 仅部署此目录。

## 来源与许可

基于 [atc1441/ATC_TLSR_Paper](https://github.com/atc1441/ATC_TLSR_Paper)，固定上游提交 `3778d296f418c30b05310d86dafa4e3404071cb4`。感谢 ATC、Telink、pvvx 及上游图形库作者。

保留应用源码原有 [许可文件](firmware/src/LICENSE) 和文件内声明。SDK、预编译 BLE 库、启动代码和工具链不重新托管，由准备脚本从固定上游版本取用，须遵守各自许可。详见 [来源说明](THIRD_PARTY_NOTICES.md)。

## 使用风险

仅针对文档中已确认的硬件映射，不保证其他 TLSR8359 价签兼容。烧录会覆盖已有程序，应先备份自己的设备。项目不含任何设备 Flash 备份、工厂固件、个人日志或密钥。当前 BLE 写入/继承的 OTA 路径没有生产级访问认证，勿用于未经授权的设备或当作安全的商用方案。
