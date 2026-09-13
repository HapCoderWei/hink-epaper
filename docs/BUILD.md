# 编译、测试与烧录

## 主机测试

安装 C99 编译器与 bash，仓库根目录执行 `bash tests/run.sh`。测试抽取当前生产 epd.c、epd_spi.c 和 led.c 的函数，检查上电无刷新、睡眠后不拉低复位、黑红各 4000 字节、BUSY 输入恢复、绿灯关闭、重入拒绝与超时清理。它不证明硅片时序或电流。

## 固件编译

需要 Git、GNU make、Python 3 及能运行上游 TC32 工具链的 Linux x86_64 环境。Mac 上可使用 Linux x86_64 虚拟机，不能把 Linux 编译器直接当作 macOS 程序运行。

确认上游 SDK/工具链许可适合你的用途后，在仓库根目录执行：

```sh
bash scripts/prepare-build.sh
cd build/firmware
make -j4 OUT_PATH=./out_b1 PROJECT_NAME=HINK_POWER_B1_ALWAYS_POWERED DIAG_ADV_UNITS=1600
sha256sum HINK_POWER_B1_ALWAYS_POWERED.bin
```

当前 M1 构建流程会在 Telink 工具追加原生 CRC 后，再自动追加 24 字节 HINK OTA 清单并独立复验。`HINK_FW_VERSION` 是清单中的 32 位版本整数；需要让两个候选版本可区分时，应显式传入不同且单调递增的值：

```sh
make -j4 OUT_PATH=./out_m1 PROJECT_NAME=HINK_OTA_V2_M1 HINK_FW_VERSION=1
python3 tools/ota_manifest.py inspect HINK_OTA_V2_M1.bin
python3 tools/ota_manifest.py verify HINK_OTA_V2_M1.bin
```

当前测试板往返验收使用版本 2、3、4；每次正式生成候选文件都必须提高 `HINK_FW_VERSION`，并在网页安装前核对当前版本、候选版本和目标槽。OTA v2 固件首次仍需通过 SWire 写入；一旦设备已经运行 M1-C 或更新固件，后续受控升级可以只走 BLE，不要求烧录器在升级时保持连接。

M2 真机验收准备了同源的 v5 SWire 基线和 v6 BLE 候选。必须先让板上运行带恢复逻辑的 v5，再由 v5 安装 v6；若让现有 M1/v4 直接安装 v5，安装动作由旧代码执行，不会生成 M2 备份和日志，不能算作 M2 验收。M2-C 当前产物与哈希记录在 [M2 实施方案](MILESTONE_OTA_V2_M2.md)。

`HINK_TRIAL_FAIL_TEST=1` 只用于生成会故意触发试运行看门狗的 M2-D 回滚测试镜像，普通构建必须保持默认值 0。不要把带 `FAIL` 的产物当作日常固件或发布固件。

板型默认固定为 `0x213a`。清单工具会拒绝缺少或损坏 Telink 原生尾部 CRC 的输入，防止构建步骤顺序错误。

脚本从固定上游提交提取 SDK、启动/链接代码、构建辅助文件和 Linux 工具链，然后叠加本仓库 B1 源码和 makefile。已检查这些依赖与 B1 本地已验证工程一致。SDK 目标 `CHIP_TYPE_8258` 是本工程在 TLSR8359 上实测使用的兼容配置，不要凭名字自行更换。

准备脚本拒绝覆盖已有 `build/firmware`，要重建可先把旧目录改名保留，再重新准备。改变参数时用新的 OUT_PATH，避免旧目标文件混用。

B1 归档版本在引入 M1 清单前的基准长度为 58,536 字节；基准 SHA-256：`6c8ed7cab389061f3424cac18a1401815ee55d129a69705da4e7beb164810b40`。当前源码的新构建会比原生 Telink payload 多 24 字节清单，不能再直接与该旧哈希比较。不同工具链产生的文件不能默认与基准等价。发布不含设备读回镜像。

发布复验（2026-09-05）：使用脚本拉取固定上游依赖，在 Linux x86_64 全新输出目录重新编译，所得 SHA-256 与上述板上已验证 B1 完全相同。脚本提前补齐工具链执行权限，避免上游 make 的 chmod_all 与并行编译竞争。未再次烧录设备。

## 写入与回退原则

使用支持 TLSR825x/8359 的双向 SWire 工具，已验证为 pvvx TLSRPGM 方案。先按 [硬件说明](HARDWARE.md) 接线，确认供电和芯片识别，读取并保存你自己的原始 Flash；无法读回时不要把“发送完成”当作烧录成功。

B1 写入地址为 `0x000000`，只擦写覆盖程序的扇区，不整片擦除。写入后读回 58,536 字节，与待写文件逐字节比较，再复位验证 BLE、传图和光学刷新。此操作会覆盖原程序，保护好自己的备份。当前项目网页包含折叠的 OTA v2 实验区；只有通过新协议握手、清单校验和两步确认才会切换启动槽，绝不能用旧 ATC OTA 页面替代。
