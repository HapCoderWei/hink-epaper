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

脚本从固定上游提交提取 SDK、启动/链接代码、构建辅助文件和 Linux 工具链，然后叠加本仓库 B1 源码和 makefile。已检查这些依赖与 B1 本地已验证工程一致。SDK 目标 `CHIP_TYPE_8258` 是本工程在 TLSR8359 上实测使用的兼容配置，不要凭名字自行更换。

准备脚本拒绝覆盖已有 `build/firmware`，要重建可先把旧目录改名保留，再重新准备。改变参数时用新的 OUT_PATH，避免旧目标文件混用。

基准长度 58,536 字节；基准 SHA-256：`6c8ed7cab389061f3424cac18a1401815ee55d129a69705da4e7beb164810b40`。不同工具链产生的文件不能默认与基准等价。发布不含设备读回镜像。

## 写入与回退原则

使用支持 TLSR825x/8359 的双向 SWire 工具，已验证为 pvvx TLSRPGM 方案。先按 [硬件说明](HARDWARE.md) 接线，确认供电和芯片识别，读取并保存你自己的原始 Flash；无法读回时不要把“发送完成”当作烧录成功。

B1 写入地址为 `0x000000`，只擦写覆盖程序的扇区，不整片擦除。写入后读回 58,536 字节，与待写文件逐字节比较，再复位验证 BLE、传图和光学刷新。此操作会覆盖原程序，保护好自己的备份。通用网页传图工具不执行固件烧录。

