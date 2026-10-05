# 2026-10-03 第二轮性能优化与板端对比

本轮基线为上一轮已部署并提交的 `c1a0b0d66950d6c1821703b1ca2010d10f8d9c08`。从干净工作区冻结源码、重新构建后，ARM 基线 SHA-256 与板上原程序完全一致。本报告只比较第二轮新增的优化，不把上一轮收益重复计入。

6 段实际 PCM 持续编辑时，CPU 41.692% → 39.188%，相对降低 6.01%。32 段实际 PCM 持续编辑时，CPU 50.595% → 47.246%，相对降低 6.62%。

保留了底栏排版缓存与控制点透明边缘裁剪；曲线替代绘制和控制点组合缓存经过实验后撤回。最终版本已安装至 `root@192.168.123.100:/opt/qdsssp/bin/qdsssp_demo`，恢复默认 PCM、30 fps 演示。

## 实际保留的改动

- 底栏数字变化且字形位置保持一致时，复用 `QTextLayout` 生成的字形与位置，替换数字 glyph。模板最多 32 项，字体、DPI、DPR、文字方向变化时清空；数字需等宽且相邻数字/标点无 kerning。特殊字距、装饰、复杂排版、RTL、宽度不足等情况回退原排版处理。测试逐像素对照原 `elidedText` + `drawText`。
- Q 滑条局部恢复区域未覆盖固定 `Q` 标签实际字形边界时，跳过这次标签排版绘制；完整恢复仍正常绘制。
- 控制点缓存生成后，在 DPR=1 时一次性扫描并裁去透明边缘，保留原中心偏移、编号、抗锯齿及绘制顺序。后续合成读取更小图像。其他 DPR 使用原完整图像，避免裁剪后的图像边界采样差异。

保持原双精度响应、0.2 个物理像素折线误差上限、EQ 抗锯齿、滤波器范围和输入交互。未修改 FFT、频谱邮箱及生产源。

## 没有保留的候选

| 候选 | 实验结果 | 决定 |
| --- | --- | --- |
| 路径填充改 `drawPolygon` | 117 个像素对照场景一致；ARM 隔离填充约 1.47 ms，收益很小 | 保留原路径 |
| 曲线描边改 `drawPolyline` | 单段 / 汇总分别 6 / 18 个场景出现像素差异；ARM 没有稳定明显收益 | 撤回 |
| 曲线叠到独立透明缓存 | 117 个场景中 111 个像素不同，额外 alpha 合成引入整数舍入差异 | 撤回 |
| Gain 平直填充改矩形 | 21 个场景中 8 个像素不同；直线描边收益很小 | 撤回 |
| 多个静止控制点组合缓存 | 32 段 PCM 初筛 CPU 47.446% → 48.195%，绘制均值 8.078 → 8.283 ms | 撤回 |

组合缓存初筛只运行一组配对，足以说明该候选不值得在本轮保留，不能用作最终收益统计。其基线已含底栏/裁剪优化，不能与下面的正式基线混用。隔离曲线测试也不代表完整组件性能。

## 测试条件

- RK3506，3× Cortex-A7，ARMv7/NEON，内核 6.1.99，Qt 5.15.19 `linuxfb`；800×480 / 32 bpp，演示 EQ 组件 800×424，DPR=1，时钟记录 1.2 GHz。
- 同一 SDK / Qt / Release 配置，GCC 12.4，`-O3 -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard`，PipeWire 支持开启。正式测试使用实际 PCM 或确定性合成源，不依赖实时 PipeWire 服务。
- 正式基准限制到 CPU 0，PCM 工作线程继承相同亲和性；停止普通演示，串行比较，每场景三轮，第二轮互换两个版本顺序。
- 每次预热 60 次，统计 180 次输入更新，末尾含原有 100 ms 排空。30 fps：6/32 段 × 合成动态、PCM 动态、PCM 静态、EQ-only，共 48 次；60 fps：6/32 段 × PCM 动态/静态，共 24 次。
- 表格取三轮统计量的中位数，分位数不跨运行拼接。CPU 100% 表示占满一个核；`paint_fps` 表示软件绘制吞吐，未测面板扫描或触摸到显示延迟。
- 板端时钟未校时；报告日期取工作站日期，计时使用单调时钟。

## 30 fps 正式结果

| 场景 | 基线 CPU % | 优化 CPU % | 相对变化 | 基线 / 优化绘制 fps |
| --- | ---: | ---: | ---: | ---: |
| b32-eq-only | 40.118 | 37.058 | -7.63% | 29.470 / 29.631 |
| b32-pcm-dynamic | 50.595 | 47.246 | -6.62% | 29.845 / 29.840 |
| b32-pcm-static | 18.737 | 18.795 | +0.31% | 29.820 / 29.828 |
| b32-synthetic-dynamic | 45.282 | 41.981 | -7.29% | 29.639 / 29.631 |
| b6-eq-only | 31.690 | 29.972 | -5.42% | 29.467 / 29.630 |
| b6-pcm-dynamic | 41.692 | 39.188 | -6.01% | 29.946 / 29.777 |
| b6-pcm-static | 18.660 | 18.884 | +1.20% | 29.742 / 29.747 |
| b6-synthetic-dynamic | 36.764 | 34.944 | -4.95% | 29.627 / 29.463 |

| 段数 / PCM 动态 | Paint 均值 ms | Paint P95 / P99 ms | Prepare P95 ms | Footer 均值 ms | Controls 均值 ms | Curves 均值 ms |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| b32-pcm-dynamic 基线 | 8.513 | 10.917 / 11.704 | 4.134 | 1.766 | 1.027 | 5.627 |
| b32-pcm-dynamic 优化 | 8.087 | 10.081 / 11.144 | 3.341 | 1.008 | 0.538 | 5.658 |
| b6-pcm-dynamic 基线 | 5.389 | 7.307 / 8.179 | 4.059 | 1.709 | 0.249 | 3.253 |
| b6-pcm-dynamic 优化 | 5.298 | 7.167 / 8.193 | 3.313 | 1.045 | 0.158 | 3.243 |

## 60 fps 补充结果

| 场景 | 基线 CPU % | 优化 CPU % | 相对变化 | 基线 / 优化绘制 fps |
| --- | ---: | ---: | ---: | ---: |
| b32-pcm-dynamic | 94.338 | 89.229 | -5.42% | 59.291 / 59.845 |
| b32-pcm-static | 35.855 | 36.191 | +0.94% | 59.396 / 59.461 |
| b6-pcm-dynamic | 77.323 | 73.759 | -4.61% | 59.197 / 59.183 |
| b6-pcm-static | 35.851 | 35.684 | -0.47% | 59.126 / 59.177 |

| 段数 / PCM 动态 | Paint 均值 ms | Paint P95 / P99 ms | Prepare P95 ms | Footer 均值 ms | Controls 均值 ms | Curves 均值 ms |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| b32-pcm-dynamic 基线 | 8.638 | 11.220 / 12.082 | 4.121 | 1.755 | 1.091 | 5.693 |
| b32-pcm-dynamic 优化 | 8.195 | 10.548 / 11.441 | 3.436 | 1.024 | 0.587 | 5.738 |
| b6-pcm-dynamic 基线 | 5.624 | 7.617 / 8.404 | 4.107 | 1.731 | 0.276 | 3.389 |
| b6-pcm-dynamic 优化 | 5.526 | 7.451 / 8.403 | 3.278 | 1.000 | 0.175 | 3.382 |

动态编辑是本轮优化的主要适用场景。静态 EQ 在预热后不更新底栏，曲线与控制点缓存也无需重建，因此不应预期相同收益；静态 CPU 变化按表中实际数值报告。曲线绘制仍是动态场景的主要开销，当前候选不足以在保持像素输出的同时稳定降低它。正常部署继续使用 30 fps。

72 次正式运行中，48 次实际 PCM 的 `pcm_dropped_samples` 全部为 0。所有场景中位峰值 RSS 变化为 -480～+80 KiB，受字体初始化及分配波动影响，完整样本见 JSON。

## 功能和部署验证

- 最终主机 CTest 通过；1.5× / 2× 全套测试通过，各 27 项（含初始化与清理）。新增数字布局缓存对照和控制点裁剪像素对照；包括 1× / 1.25× / 1.5× / 2× 图像及小数坐标、选中状态、双位编号。原有局部刷新、缓存状态、全部滤波器、FFT/DFT、并发一致性、鼠标及多点触摸测试继续通过。
- 两组正式板端 QtTest 均 27 项通过。原并发测试在单核调度下可能恰好无成功读取，补充生产者/消费者结束握手与最后一帧一致性检查；仅修改测试，不改变邮箱实现。
- 通过哈希检查后原子替换已测试的优化二进制。上一轮程序备份为 `/opt/qdsssp/bin/qdsssp_demo.before-round2-20261003`；更早的 `.before-20261003` 备份保留。
- 默认 PCM、30 fps 程序重新启动，PID **2427**，执行文件核验正确、进程存活、允许 CPU **0-2**。日志 `/tmp/qdsssp-demo.log`，PID 文件 `/tmp/qdsssp-demo.pid`。普通运行不限制单核。
- 下载并检查安装后 800×480 截图 `build-performance-round2/deployed-preview.png`。PCM 文件哈希不变。

| 文件 | SHA-256 |
| --- | --- |
| 基线 / 第二轮备份 | `7f6f4024c6e906aa850c0aabc25dd3aea6f64b9cb0a453869ea91c55d0ed838c` |
| 最终优化版 / 安装程序 | `7ec0f065c8a718538a01275d3a86e0b1e681ae18eb29081a43649c1fc47145ba` |
| 实际 PCM | `844595f970b1a60016c9d4b0791031ec69dfbb8ff0fe7ca27e3764c9fe87057c` |

## 复现与记录

本轮生成物位于被 `.gitignore` 排除的 `build-performance-round2/`：`baseline-source/`、`baseline-manifest.json`、`baseline-commit.txt` 固定基线；`final-source/`、`final-compiled-manifest.json` 保存最终构建相关源码和哈希，`final-source-manifest.json` 记录构建前全部跟踪文件及新头文件的哈希（文档随后更新）；`final-bundle/sha256.json` 记录测试二进制、脚本和 PCM；`board-30fps/`、`board-60fps/` 保留环境、功能测试、全部原始日志和 `results.json`；`final-board-progress.log` 记录执行进度；`deployment.log` / `deployment.json` 记录安装与进程验证。

被撤回实验保留在 `render-probe/`、`render-probe-expanded-arm.log`、`screen-footer-results/`、`screen-groups-results/`、`rejected-groups-source/`。这些初筛不计入 72 次正式运行。

```sh
# 在 final-bundle 中执行，两组串行；每场景默认三轮、预热 60 次
export QT_QPA_PLATFORM_PLUGIN_PATH=/usr/lib/qt/plugins/platforms
QDSSSP_BENCHMARK_BANDS="6 32" QDSSSP_BENCHMARK_UPDATES=180 \
  QDSSSP_BENCHMARK_SCENARIOS="synthetic-dynamic pcm-dynamic pcm-static eq-only" \
  taskset -c 0 sh ./run-board-comparison.sh
QDSSSP_BENCHMARK_BANDS="6 32" QDSSSP_BENCHMARK_UPDATES=180 QDSSSP_BENCHMARK_FPS=60 \
  QDSSSP_BENCHMARK_SCENARIOS="pcm-dynamic pcm-static" \
  taskset -c 0 sh ./run-board-comparison.sh

# 下载结果目录后在仓库中汇总
python3 tools/summarize-benchmarks.py build-performance-round2/board-30fps \
  --output build-performance-round2/board-30fps/results.json
python3 tools/summarize-benchmarks.py build-performance-round2/board-60fps \
  --output build-performance-round2/board-60fps/results.json
```
