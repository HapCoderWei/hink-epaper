# 网页使用与部署

入口：https://hapcoderwei.github.io/hink-epaper/

`site/index.html` 是单文件纯前端，没有接口、数据库、外部 JS 或密钥。文件选择、缩放/裁切、三色量化在浏览器 Canvas 内完成，然后通过本机蓝牙发送；不会将选择的图片上传到 GitHub。GitHub 仍会接收到加载网页的普通 HTTP 请求。

1. 用支持 Web Bluetooth 的浏览器打开 HTTPS 网址，推荐已实测的 Mac Chrome。
2. 开启蓝牙并允许操作系统/浏览器蓝牙权限；价签供电且在附近。
3. 点击连接，选择 HINK（或旧缓存 ESL）；新站点来源需要重新授权，不能继承 localhost 授权。
4. 选图、选择裁切/留白，确认预览，再发送。测试图和全白按钮只在连接成功后可用。
5. 等屏幕完整刷新，再关闭网页；当前未实现自动断开，长期连接明显增加耗电。

这不是远程互联网价签网关。任意手机/Safari 不保证支持此 API；以浏览器 Web Bluetooth 支持情况为准。页面需要 HTTPS 或本机安全开发来源，普通局域网 HTTP 不可靠。当前无 Service Worker，不承诺断网后重新打开网页可用。

GitHub Pages 通过 `.github/workflows/pages.yml` 发布 `site/`，不会部署固件和硬件文档目录。站点公开不等于跨互联网能直接控制蓝牙，但附近的人可能用兼容客户端写入未认证价签。

参考：[GitHub Pages](https://docs.github.com/en/pages/getting-started-with-github-pages/configuring-a-publishing-source-for-your-github-pages-site)、[Chrome Web Bluetooth](https://developer.chrome.com/docs/capabilities/bluetooth)。

