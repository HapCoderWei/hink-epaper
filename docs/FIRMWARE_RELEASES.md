# 固件分享页与 GitHub Release 发布流程

公开固件入口：<https://hapcoderwei.github.io/hink-epaper/firmware.html>

这个页面部署在现有 GitHub Pages 中，只展示经过人工筛选的完整固件，不枚举仓库里所有 GitHub Releases。M0、M1、故障注入、断电断点和其他中间验收镜像即使曾用于开发，也不会进入面向爱好者的下载页。

当前唯一推荐版本定名为 **HINK E-Paper Community v1.0.0**，设备内部版本仍为 `19`，对外附件名为 `HINK_E0213A162_Community_v1.0.0.bin`。二进制内容不因改名而改变：长度 61,320 字节，SHA-256 为 `126b37fc516e76f103131e0595a36112ff828ded8f372021adb562d3f30bbe97`。

## 发布前边界

- 只发布普通固件构建。任何文件名或构建参数含 `FAIL`、`POWER`、`SW_RESET`、`PHYSICAL_CUT` 等故障注入或断点诊断用途的镜像不得发布。
- 后续正式版的 `HINK_FW_VERSION` 必须比 19 更大。Git 标签（例如 `v1.1.0`）和设备内部整数版本（例如 `21`）是两个字段，两者都写进 Release 说明。
- 使用 `firmware/tools/ota_manifest.py verify` 检查 HINK 清单、板型、长度、payload CRC32、清单 CRC32 和 Telink 原生 CRC。
- 运行 `bash tests/run.sh`；它是主机回归，不替代真机传图、光学刷新、断开重连和必要的功耗复测。
- 正式附件只放可公开的应用固件，不放设备 Flash 读回、原厂备份、MAC、日志或救援现场镜像。
- 新固件只有在功能完整且验证记录达到发布要求时，才替换页面上的唯一推荐项。需要先收集反馈时可建立 GitHub pre-release，但不把它加入分享页。

## 推荐发布步骤

1. 用全新的构建输出目录生成普通固件，显式设置新的 `HINK_FW_VERSION`；不要复用故障注入测试目录。
2. 检查清单并运行完整主机测试。
3. 计算 SHA-256，记录文件长度；推荐把附件命名为 `HINK_E0213A162_v<设备内部版本>.bin`。
4. 在可 SWire 救援的测试价签上完成与本次改动相称的真机验证，并把“已测”和“未测”分开记录。
5. 在 GitHub 仓库 Releases 中选择 “Draft a new release”，创建新的语义化标签并上传改名后的 `.bin`。
6. 使用下方模板填写版本说明。发布前再次确认附件不是全 Flash 读回文件，也不是诊断镜像。
7. 发布后打开固件分享页，确认固件名称、附件、大小、SHA-256、首次接线图和两套更新步骤均正确。
8. 实际下载一次附件，在本地重新计算 SHA-256；这验证公开下载链路，不只是 GitHub 页面文字。

## Release 说明模板

```markdown
## 适用硬件

- HINK-E0213A162-FPC-A0
- TLSR8359F512ET32
- Board ID：0x213A

## 版本身份

- GitHub 版本：v1.1.0
- 设备内部版本：v21
- 文件：HINK_E0213A162_v21.bin
- 长度：<字节数>
- SHA-256：<64 位摘要>

## 本版变化

- <用户可感知的变化>
- <修复内容>

## 验证结果

- [x] HINK 清单和 Telink CRC 校验
- [x] `bash tests/run.sh`
- [ ] SWire 写入与逐字节回读（未执行时必须保留未勾选）
- [ ] BLE OTA 安装与重连版本核对
- [ ] 黑、白、红完整刷新目视确认
- [ ] 待机功耗复测

## 更新方法

- 已运行 OTA v2 M2：使用项目 Pages 的“固件维护”区上传、板端校验并完成两步安装。
- B1、v0.1.x 或版本未知：先备份设备，再通过 SWire 从 0x000000 首次写入；禁止使用旧 ATC OTA 页面。

## 已知限制

- OTA v2 M2 没有固件签名或身份认证，只能在可信的近距离 BLE 环境使用。
- 没有独立不可变 bootloader；极端损坏仍可能需要 SWire。
```

## 页面与测试文件

- `site/firmware.html`：可直接分享的单一推荐固件、接线图、首次烧录和 OTA 更新说明页。
- `site/firmware.css`：独立的响应式电子纸视觉样式与接线图样式。
- `site/assets/firmware/`：适配价签正反面、P6 引脚实物图和刷机后显示效果。
- `site/index.html`：传图与 OTA 工具，顶部和固件维护区都链接到分享页。
- `tests/web_ui_test.js`：检查两个页面的关键入口、唯一固件、接线关系、安全提示和 JavaScript 语法。

固件分享页只说明首次 SWire 烧录所需的硬件连接、安全条件和操作顺序，不在浏览器中实现烧录器控制。使用者自行准备兼容 TLSRPGM 工具；设备成功运行 HINK 固件后，才使用网页完成蓝牙传图和 OTA。
