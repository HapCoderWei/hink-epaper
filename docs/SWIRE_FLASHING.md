# FPC-A002 价签 SWire 快速烧录手册

本手册用于已经确认硬件为 `FPC-A002 / SSD1680` 的价签。目标是以后重复烧录时直接采用已经验证的稳定参数，不再先读取或备份整片 Flash，也不再重复尝试多组复位和速率参数。

## 1. 什么时候需要备份 Flash

以下情况才考虑在写入前读取并保存整片 512 KiB Flash：

- 第一次接触一种此前没有验证过的价签、PCB 或屏幕；
- 正在确认 SWire 接线、供电或 Flash 型号；
- 需要分析、逆向或保留原厂固件；
- 设备中存在尚未另行保存的重要数据。

对于已经确认属于 `FPC-A002 / SSD1680` 的普通价签，日常烧录直接覆盖应用区，不再先做整片 Flash 备份。烧录工具只擦除并改写固件文件覆盖的 `0x000000` 起始区域，不执行整片擦除，高地址 BLE 身份与校准数据保持不变。

写入后的同长度读回校验仍然保留。它用于确认本次烧录没有传输或写入错误，与写入前备份不是一回事。

## 2. 固定硬件与固件

- 屏幕排线：`FPC-A002`，常见日期丝印 `20.04.08`
- 屏幕控制器：SSD1680 兼容三色屏
- MCU：TLSR8359，TLSR825x 兼容调试接口
- 固件 Board ID：`0x2198`
- 当前已验证固件：`build/firmware/HINK_GDEY0213Z98_V20.bin`
- 固件长度：61,320 字节
- SHA-256：`7e598c3a4c6672dba1b9b8e85559f2df81e37de5208020bbfc2e0011f88ba78e`

不要给该屏幕写入 Board ID `0x213A` 的 HINK A162 固件。

## 3. 接线

| TLSRPGM | 价签 |
|---|---|
| SWM | P6-3 / SWS |
| GND | P6-1 / GND |
| 3.3 V | P6-5 / VCC |

只使用一套稳定的 3.0～3.3 V 电源。不要同时连接电池和外部供电，不要接 5 V。

## 4. 已验证的稳定参数

2026-09-23 的价签需要比早期样品更长的复位窗口和更低的 SWire 速率。后续统一直接使用这一组更保守的参数：

- 串口：`/dev/cu.usbmodem86FB04741`，每次使用前仍应确认实际端口；
- UART：1,500,000 bit/s；
- 硬复位：500 ms；
- 激活窗口：15,000 ms；
- SWire 分频：60，约 0.1067 Mbit/s；
- 不使用 `-c / CPU Stall`。

## 5. 日常直接覆盖烧录

先确认固件身份和哈希：

```sh
cd /Users/wei/Development/hink-epaper

python3 build/firmware/tools/ota_manifest.py verify \
  build/firmware/HINK_GDEY0213Z98_V20.bin --board 0x2198

shasum -a 256 build/firmware/HINK_GDEY0213Z98_V20.bin
```

预期清单显示 `board=0x2198`、`firmware_version=20`，SHA-256 与第 2 节一致。

确认端口后直接覆盖写入，不做写入前 Flash 读取：

```sh
cd /Users/wei/Development/hink-epaper

HINK_PY=/Users/wei/.platformio/penv/bin/python3
HINK_PGM=/Users/wei/Documents/Codex/2026-08-29/referenced-chatgpt-conversation-this-is-an/work/TLSRPGM/TlsrPgm.py
HINK_PORT=/dev/cu.usbmodem86FB04741

"$HINK_PY" "$HINK_PGM" \
  -w -p "$HINK_PORT" -b 1500000 -t 500 -a 15000 -d 60 \
  we 0x000000 build/firmware/HINK_GDEY0213Z98_V20.bin
```

只有看到每个扇区正常执行 `Erase` 和 `Write`，并最终显示 `Done`，才进入下一步。

## 6. 写后读回校验并启动

读回长度必须与固件实际长度一致。v20 固件当前为 61,320 字节：

```sh
mkdir -p output/HINK_FPC_A002_FLASH_CHECK

"$HINK_PY" "$HINK_PGM" \
  -w -p "$HINK_PORT" -b 1500000 -t 500 -a 15000 -d 60 \
  rf 0x000000 61320 \
  output/HINK_FPC_A002_FLASH_CHECK/HINK_GDEY0213Z98_V20_readback.bin

cmp \
  build/firmware/HINK_GDEY0213Z98_V20.bin \
  output/HINK_FPC_A002_FLASH_CHECK/HINK_GDEY0213Z98_V20_readback.bin

shasum -a 256 \
  build/firmware/HINK_GDEY0213Z98_V20.bin \
  output/HINK_FPC_A002_FLASH_CHECK/HINK_GDEY0213Z98_V20_readback.bin
```

`cmp` 没有输出且两个 SHA-256 完全相同，才启动 CPU：

```sh
"$HINK_PY" "$HINK_PGM" \
  -w -r -p "$HINK_PORT" -b 1500000 -t 500 -a 15000 -d 60 \
  df 0x000000 16
```

成功时启动头应包含 `KNLT`，最后显示 `CPU Run... ok`。

## 7. 烧录后的最小确认

1. 网页能够搜索到新的 `HINK_XXXXXX` 设备；
2. 网页能够连接并发送图片；
3. 屏幕正常完成黑、白、红三色刷新；
4. OTA 页面选择 `FPC-A002 / SSD1680 三色屏`，不要选择 HINK A162。

## 8. 常见故障只按这个顺序处理

### `SWire read timeout` 或 `Activate ... Error`

1. 确认 `SWM → SWS`；
2. 确认共地；
3. 测量价签 VCC 确实为稳定 3.0～3.3 V；
4. 重新给价签上电；
5. 仍使用 `-t 500 -a 15000 -d 60` 重试一次，不再从高速参数开始试错。

### 诊断读取全是 `00`

原厂固件可能让 Flash 处于 Deep Power-Down。这不是加密或写保护。日常烧录正常执行 `we` 即可；只有在诊断连接时才需要连续执行两次短读取，第一次用于唤醒，第二次确认真实内容：

```sh
"$HINK_PY" "$HINK_PGM" \
  -w -p "$HINK_PORT" -b 1500000 -t 500 -a 15000 -d 60 \
  df 0x000000 32
```

不要因为第一次短读取全零就重新进行整片备份或怀疑固件有保护。

## 9. 后续固件版本

当固件文件名、版本或长度变化时：

- 保持 Board ID 为 `0x2198`；
- 先用清单工具验证文件；
- 第 6 节的读回长度改为新固件的实际字节数；
- 不要仅因为版本更新而重新备份整片原厂 Flash；
- 已运行 OTA v2 M2 的价签，日常升级优先使用网页 OTA，SWire 留作首次接管和故障救援。
