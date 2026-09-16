# 内置图集（Gallery）实施规范 · 终版

方案日期：2026-09-15 · 状态：已实施并按第 7 节验收通过（2026-09-16），首批收录 `force-logo-red` 一张

本规范面向执行 Agent：按第 8 节顺序实施，逐项满足第 7 节验收标准。术语：「固件页」= `site/firmware.html`，「工作室」= `site/index.html`。

## 1. 已确认的决策

| 项 | 决策 |
|---|---|
| 图集位置 | 固件页新增「图集」区块（`#gallery-art`），不放独立页 |
| 工作室内嵌选择器 | 不做 |
| 分类筛选器 | 不做，仅展示分类标签 |
| 单图上限 / 总数上限 | 300 KiB / 24 张 |
| 图片转换逻辑 | **不做**。图集图片一律视为已适配价签的成品：正确比例 + 高对比近三色，可直接发送 |
| 预览方式 | 卡片直接 `<img>` 展示原图（图片本身即最终效果），不做浏览器端量化处理 |
| 传图页处理管线 | 保持现状不变（工作室自身的 contain 适配 + 三色量化是发送路径的既有环节，对所有图片来源一视同仁） |

## 2. 图片规范（项目方准备图片时必须满足）

1. **宽高比精确 250:122**（ landscape ）。推荐导出 500×244（2×），允许 250×122；`build-gallery.py` 按宽高比校验（±1% 容差），比例不符直接报错拒绝收录。
2. **高对比、近三色**（白底、纯黑、纯红为主）。画廊预览是原图直出，不替图片做量化；图片本身越接近三色，预览与价签实际效果越一致。这是图片准备要求，不由脚本强制校验。
3. 格式 JPG 或 PNG，无透明通道需求（白底即可），单文件 ≤ 300 KiB。
4. 文件名用英文小写 slug（字母/数字/连字符），它就是清单 id 的来源，例如 `force-logo-red.png`。

## 3. 总体架构

```
site/
├── assets/
│   └── gallery/                 # 项目方图片 + 机器生成清单
│       ├── manifest.json        # build-gallery.py 生成，禁止手改
│       └── <slug>.jpg|png       # 已适配图片
├── firmware.html                # 新增「图集」区块（#gallery-art）
├── firmware.js                  # 图集渲染（fetch 清单 + 建卡片）
├── firmware.css                 # 图集样式
├── index.html                   # 新增 ?art=<id> 深链载入（仅此一处改动）
└── image-processing.js          # 不动

scripts/
└── build-gallery.py             # 扫描 assets/gallery/ 生成/校验 manifest.json

tests/
└── gallery_manifest_test.py     # 清单一致性回归（接入 tests/run.sh）
```

数据流：

1. 项目方放图到 `site/assets/gallery/` → 运行 `python3 scripts/build-gallery.py` 重新生成 `manifest.json` → 提交 → GitHub Pages 自动部署。
2. 访客在固件页图集区看到卡片（原图缩略 + 标题 + 分类），点击 → 跳转 `index.html?art=<id>`。
3. 工作室启动时解析 `art` 参数 → 按清单取文件 → 走**现有 `loadImageFile` 入口**载入 → 预览、连接、发送流程与本地选图完全一致。

## 4. `manifest.json` 格式

```json
{
  "version": 1,
  "images": [
    {
      "id": "force-logo-red",
      "file": "force-logo-red.png",
      "title": "Force Logo · 红色版",
      "category": "标语"
    }
  ]
}
```

规则：

- `id` = 文件名去扩展名；只允许 `[a-z0-9-]`，必须唯一。
- `file` 必须与 `id` 对应的真实文件名一致，且文件存在于同目录。
- `title`/`category` 来自脚本内置的标题映射表（见 5.3），不手改 JSON。

## 5. `scripts/build-gallery.py` 规格

Python 3 标准库实现，零依赖（对齐 `firmware/tools/ota_manifest.py` 的风格）。

### 5.1 功能

- 扫描 `site/assets/gallery/` 下所有 `.jpg/.jpeg/.png`（跳过 `manifest.json`），按文件名排序。
- 校验每个文件：扩展名、大小 ≤ 300 KiB、总数 ≤ 24、文件名 slug 合法、id 唯一。
- **宽高比校验**：读取图片头部解析尺寸（PNG 读 IHDR；JPEG 扫描 SOF0/SOF2 标记），要求 `|width/height - 250/122| / (250/122) ≤ 0.01`，否则报错并指出文件名。
- 生成 `manifest.json`（字段见第 4 节，键序稳定、`json.dumps(..., ensure_ascii=False, indent=2)` + 末尾换行），重复运行输出字节一致（幂等）。
- 默认模式写文件；`--check` 模式只校验不写盘，退出码非零即失败。

### 5.2 标题映射表（脚本内置常量）

文件名 slug → `{title, category}` 的 dict。新图片入库时项目方在此表加一行；表里没有的 slug 报错退出（防止"放了图忘了登记"）。

### 5.3 命令

```sh
python3 scripts/build-gallery.py          # 生成清单
python3 scripts/build-gallery.py --check  # 校验（CI/测试用）
```

## 6. 页面改动规格

### 6.1 固件页图集区块（`site/firmware.html`）

- 导航 `.nav-links` 中「OTA 更新」之后加：`<a href="#gallery-art">图集</a>`。
- 在 `<section id="ota">` 结束之后、`<section id="gallery">` 之前插入：

```html
<section id="gallery-art" class="shell section art-section">
  <div class="section-head">
    <div><p class="section-label">BUILT-IN ART</p><h2>内置图集</h2></div>
    <p>为这块价签提前适配好的图片，效果即所见。点开任意一张进入传图工具，连接价签即可发送。</p>
  </div>
  <div id="art-grid" class="art-grid" aria-live="polite">
    <p class="art-loading">图集加载中…（若长时间无内容，请确认通过 HTTPS 页面访问）</p>
  </div>
</section>
```

- 卡片由 `firmware.js` 依据 `manifest.json` 构建：整卡是一个 `<a href="index.html?art=<id>">`，内含 250:122 比例缩略框（`<img loading="lazy" src="assets/gallery/<file>" alt="<title>">`，`object-fit: contain`，白底）、标题、分类标签。
- 加载失败（fetch 失败 / 清单为空 / JSON 损坏）：`#art-grid` 内替换为静态提示句「内置图集暂时不可用，可直接打开传图工具上传自己的图片。」，不影响页面其他部分。
- **CSP 红线**：固件页禁止内联脚本与内联样式；新 DOM 一律 `createElement` + `className`，样式只进 `firmware.css`。
- 不引入 `image-processing.js`（预览不做量化）。

### 6.2 深链载入（`site/index.html`）

唯一改动点：在页面既有 IIFE 内、`loadImageFile` 函数定义之后（该函数在文件约 1117 行，以实际代码为准）新增深链逻辑：

1. `new URLSearchParams(location.search).get('art')`；为空则什么都不做（现状不变）。
2. fetch `assets/gallery/manifest.json` → 按 `id` 查表；找不到则 `setStatus('图集图片不存在或已下架。')` 并返回。
3. fetch `assets/gallery/<file>` → `response.blob()` → `new File([blob], file, { type: blob.type })` → 调用现有 `loadImageFile(fileObject)`。此后预览文件名显示为图片文件名，旋转/适配/发送控件行为与本地选图一致。
4. 任一步 fetch 失败：`setStatus('图集载入失败，请通过 HTTPS 页面重试，或直接选择本地图片。')`，不影响占位图状态。
5. 不做 `history.replaceState` 清参数（保留可转发链接）。
6. 工作室其余逻辑（连接、OTA、空闲断开）一律不碰。

### 6.3 样式（`site/firmware.css`）

新增 `.art-section / .art-grid / .art-card / .art-thumb / .art-meta` 等类，视觉语言沿用现有图库卡片（白底圆角卡片、顶部黑/白/红刻度条可用 `.screen-ruler` 同款渐变、mono 小字标签）。响应式：桌面 3–4 列网格，`≤940px` 两列，`≤660px` 单列。`image-rendering` 保持默认（不做像素化）。

## 7. 测试与验收

### 7.1 自动化测试

**新增 `tests/gallery_manifest_test.py`**（加入 `tests/run.sh`，放在 `ota_manifest_test.py` 之后）：

- 以 subprocess 运行 `python3 scripts/build-gallery.py --check`，要求退出码 0。
- 解析 `site/assets/gallery/manifest.json`：schema 完整、`id` slug 合法且唯一、`file` 与 `id` 同名、文件存在、无未收录图片。

**`tests/web_ui_test.js` 增量断言**（保留全部既有断言）：

- `firmwareHtml` 含 `id="gallery-art"`、`assets/gallery/manifest.json`、`index.html?art=`；
- `index.html` 源码含 `URLSearchParams` 与 `assets/gallery/manifest.json`；
- 固件页仍无内联脚本/内联样式（沿用现有两条 CSP 断言）。

`tests/run.sh` 全绿才算通过。

### 7.2 人工验收清单（执行 Agent 完成后逐项自查并截图）

1. 桌面（1440px）与移动（390px）截图：固件页图集区卡片布局正常、缩略图不变形、无横向溢出。
2. 点击卡片跳转 `index.html?art=force-logo-red`：预览区已载入该图，文件名显示正确；控制台无 CSP/资源错误。
3. 清单损坏场景（临时把 manifest.json 改名）：固件页图集区显示静态提示句、页面其余部分正常；恢复文件后正常。
4. 无效参数 `index.html?art=nope`：工作室保持占位图并提示「图集图片不存在或已下架。」。
5. 工作室原有功能（本地选图、连接按钮存在性）未被深链改动破坏——由 `web_ui_test.js` 既有断言兜底。

## 8. 实施顺序

1. **首批图片入库**：把 `/Users/wei/Downloads/forcelogo-red.png` 一次性处理为合规图（macOS 内置 `sips` 即可）：

   ```sh
   sips --resampleWidth 500 /Users/wei/Downloads/forcelogo-red.png --out /tmp/force-500.png
   sips --cropToHeightWidth 244 500 /tmp/force-500.png --out site/assets/gallery/force-logo-red.png
   ```

   原图 1715×917 比例 1.87 ≠ 2.05，必须先裁到 250:122，否则会被脚本拒收。注意：`sips` 不做三色量化，此图含抗锯齿杂色，发送时由工作室既有管线兜底；预览即原图。若对预览纯净度不满意，项目方可在图像编辑器里先行做一遍三色化处理（规范外的人工步骤）。
2. `scripts/build-gallery.py`（内置 `force-logo-red` → `{title: "Force Logo · 红色版", category: "标语"}` 映射），生成清单。
3. `tests/gallery_manifest_test.py` + `tests/run.sh` 接线 + `tests/web_ui_test.js` 增量断言。
4. `site/firmware.html` 导航与区块骨架；`site/firmware.js` 图集渲染；`site/firmware.css` 样式。
5. `site/index.html` 深链载入。
6. 跑 `bash tests/run.sh`，再按 7.2 做浏览器验收并附截图。

## 9. 非目标（本期明确不做）

访客上传、图集管理界面、分类筛选、工作室内嵌选择器、浏览器端图片转换/量化、深色模式适配、收藏与历史。
