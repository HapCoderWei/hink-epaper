# 来源与许可边界

本项目是 ATC_TLSR_Paper 的板级适配，不是独立重新实现全部 BLE 栈。

- 上游：https://github.com/atc1441/ATC_TLSR_Paper
- 基准提交：`3778d296f418c30b05310d86dafa4e3404071cb4`
- `firmware/src` 保留上游 MIT 许可文本及文件内作者/许可声明，包括 OneBitDisplay、TIFF_G4 等内容；继承资源不主张为本项目原创。
- `components`、`make`、`static_src`、`tc32_linux` 不放入本仓库。部分 Telink 文件带有单独的商业/保密许可说明，不能因为上游可下载就将整个 SDK 宣称为 MIT。使用者应自行确认所用 SDK 和工具链的适用许可。
- 准备脚本只在本机创建被 Git 忽略的构建目录，从上述固定提交复制依赖并叠加本项目应用代码。
- 本阶段发布源码及已验证固件的哈希，不重新托管 SDK/工具链二进制或设备读回镜像。

