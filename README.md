# QDsssp

Qt 5.15.19 / C++14 的触屏 EQ 和频谱组件，面向 RK3506，使用 `QWidget + QPainter` 软件绘制。演示窗口为 **800×480**。组件默认接收已有频谱；可选的 PipeWire 音频源在独立线程采集 DAC 监视流并计算 FFT。

已实现：

- 对数频率轴、独立 EQ dB / 频谱 dBFS 轴、总响应和选中段响应；选中段支持半透明面积阴影。
- 最多 32 段；演示默认 6 段。实际产品段数通过 `setBands()` 决定。
- Flutter DSSSP 的全部 13 种类型、类型编号和 Q 约定，以及原始图标字体。
- 单指拖点调频率和增益；双指张开降低 Q、收拢提高 Q；底部 48 px 高 Q 滑条。
- 10 点触摸输入中跟踪操作点和第二根手指，其余触点不改变当前手势；不同时编辑十个 EQ 段。切换主手指、双指切单指时重建基准，避免跳变。
- 鼠标拖动、滚轮调 Q；方向键调频率/增益，`+/-` 调 Q，Shift 细调。
- 工作线程可提交的有限容量频谱缓冲；缓存网格、EQ 位图和各段数值响应。

当前采用两层位图缓存：背景（含底部参数栏）与 EQ 覆盖层（曲线和控制点）。频谱直接绘制。

## 为什么默认不用 QCustomPlot

本组件只需要固定坐标轴、少量曲线和专用触摸交互。自绘便于限制采样数量、缓存范围及重绘频率，并减少依赖。QCustomPlot 本身也支持自适应采样、文字缓存和缓冲图层，不能仅凭库大小断言它更慢；这里选择自绘是针对功能范围的工程取舍，尚未做同一块 RK3506 上的两套实现对比。

当前 `/opt/qt5.15.19-arm` 提供 LinuxFB / minimal / offscreen / VNC 平台插件；本实现不依赖 OpenGL，也不依赖 QCustomPlot。这里使用 QWidget + QPainter，是为了直接控制采样数量、缓存范围和重绘频率。

## 构建和运行

本机 Qt 5.15.19：

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/home/kinho/Qt/5.15.19/gcc_64
cmake --build build -j4
ctest --test-dir build --output-on-failure
./build/qdsssp_demo
```

只集成组件库时关闭演示和测试目标：

```bash
cmake -S . -B build-lib -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/home/kinho/Qt/5.15.19/gcc_64 \
  -DQDSSSP_BUILD_DEMO=OFF -DQDSSSP_BUILD_TESTS=OFF
cmake --build build-lib -j4
cmake --install build-lib --prefix /opt/qdsssp
```

RK3506 的 CMake 交叉编译会自动检测 SDK 中的 PipeWire 头文件和库。启用硬件频谱源运行演示程序：

```bash
QT_QPA_PLATFORM=linuxfb ./qdsssp_demo --pipewire --pipewire-uac
```

`--pipewire` 捕获指定 PipeWire 播放 sink 的 monitor。对于 RK3506 的 USB 音频场景，使用 `--pipewire-uac` 捕获电脑发送到板子的 UAC 播放流；它监听 UAC sink 的 `monitor_FL/monitor_FR`，不是板子发送回电脑的 `alsa_input` capture，也不是 DAC 模拟引脚回采。

如果暂时不使用板端音频输入，可以直接循环播放内置 PCM 音频并计算频谱：

```bash
QT_QPA_PLATFORM=linuxfb ./qdsssp_demo
```

`DOGFIGHT.pcm` 为单声道、16-bit、44100 Hz，部署到板端的 `/opt/qdsssp/assets/DOGFIGHT.pcm`，频谱最高可覆盖到约 22.05 kHz。
演示程序不带参数时默认使用该文件，因此 Qt Creator 直接点击“运行”即可显示频谱；需要恢复实时 PipeWire 输入时再显式添加 `--pipewire`。

如果存在多个播放设备，也可以直接指定节点名：

```bash
./qdsssp_demo --pipewire --pipewire-target <sink-node-name>
```

默认参数为 48 kHz、双声道、2048 点 FFT、1024 点步进；窗口时长约 42.7 ms，频率分辨率约 23.4 Hz，频谱发布仍由 `SpectrumMailbox` 覆盖旧帧，不在 GUI 线程排队。

Qt Creator 直接打开 `CMakeLists.txt`。无桌面环境时可运行：

```bash
QT_QPA_PLATFORM=offscreen ./build/qdsssp_demo --screenshot /tmp/qdsssp-preview-800x480.png
```

ARM 构建使用本机已有的 Lyra Buildroot sysroot、编译器以及 `/opt/qt5.15.19-arm`：

```bash
cmake -S . -B build-arm -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=cmake/rk3506-toolchain.cmake \
  -DQDSSSP_BUILD_TESTS=OFF
cmake --build build-arm -j4
```

更换 SDK 安装位置时配置 `-DRK3506_HOST=... -DRK3506_QT=...`。产物是 `build-arm/qdsssp_demo` 和 `libqdsssp.a`。目标设备需提供与构建 sysroot 匹配的运行库、Qt 平台插件和普通文字字体。已成功交叉编译，并在实体 RK3506 上确认启动和显示正常。

在已配置 Qt 运行环境的设备上：

```bash
QT_QPA_PLATFORM=linuxfb ./qdsssp_demo --fps 30
```

通过 Qt Creator 部署时，工程已声明默认目标路径 `/opt/qdsssp/bin/qdsssp_demo`。若出现 `Cannot run: No command given.`，重新运行 CMake 并检查远程运行路径。

Qt 会通过设备已有的 evdev/libinput 配置接收触摸。必须由触摸驱动上报多点事件；仅上报模拟鼠标的环境仍可使用拖点和 Q 滑条。组件坐标为 Qt 逻辑像素，默认按设备缩放系数 1 设计 800×480 布局。

## 在业务工程中使用

```cpp
#include <qdsssp/eq_widget.h>

auto *eq = new qdsssp::EqWidget(parent);
eq->setSampleRate(48000);
std::vector<qdsssp::Band> bands(8);
bands[0].frequency = 1000;
bands[0].gainDb = 6;
bands[0].q = 1.4;
eq->setBands(bands);
eq->setFrameRate(30);

// No band is selected automatically. Pass -1 to clear a selection;
// tapping empty plot space also clears it.
eq->setSelectedBand(-1);

connect(eq, &qdsssp::EqWidget::bandEdited, parent,
        [](int index, qdsssp::Band band) {
    // 交给已有参数控制层；不要在这个 GUI 回调里阻塞等待设备或音频线程。
});
```

选择状态默认为空；`setBands()` 不会自动选中某段。`selectedBand()` 返回 `-1` 表示无选中，调用 `setSelectedBand(-1)` 或点按绘图区空白处可取消选择；此时隐藏单段曲线和阴影，底部参数栏提示选择一个 EQ 点。

`bandEditStarted` / `bandEdited` / `bandEditFinished` 对应开始、连续修改、结束。`bandEditFinished` 的 `cancelled=true` 表示手势被系统取消，保留最后一次已发出的值。程序调用 `setBand()` 不发 `bandEdited`，便于处理设备参数回传。连续信号按输入事件产生，图像更新按帧率合并；设备参数下发若需限流，应在业务层合并。

选中段默认以该段颜色填充响应曲线与 **0 dB 基线**之间的区域，正、负增益分别向上、向下填充，高低通、棚式和外部系数响应同样适用。效果参考 Flutter 示例的 `FilterGradient(fill: true)`，使用纯色半透明填充。默认透明度参数为 `0.25`（0 完全透明，1 完全不透明），可按页面风格调整：

```cpp
eq->setSelectedBandFillOpacity(0.75); // 接近 Flutter 示例的填充强度
eq->setSelectedBandFillEnabled(false); // 关闭阴影，保留曲线
```

阴影复用选中段的简化曲线路径并纳入 EQ 位图缓存；只有选中段响应、选择、尺寸或开关变化才重建填充路径。修改透明度不重算响应或路径。切换选中段或关闭阴影时，局部刷新同时覆盖旧、新填充区域。

除 `SpectrumMailbox` 外所有组件和响应缓存方法都在 GUI 线程使用。若跨线程连接 `Band` 信号，组件已注册对应 Qt 元类型。不要把大量 FFT 帧排进 queued signal 队列，使用以下最新帧接口。

```cpp
// GUI 线程取得共享对象，再把它交给已有 FFT 工作线程。
auto spectrumInput = eq->spectrumInput();

// FFT 工作线程：dbBins 的生命周期只需覆盖 publish() 调用。
// N=4096 的实数 FFT 对应 2049 个单边频点。
bool accepted = spectrumInput->publish(dbBins, 2049, 48000.0);
// false 表示输入已暂停、格式不合法或短暂锁竞争，允许丢掉这帧。
```

频谱协议：

| 项目 | 约定 |
| --- | --- |
| 容量 | 2～8193 个单边频点，最多对应 N=16384 的实数 FFT |
| 频率 | 等间隔，包含 DC 和 Nyquist，`f[k] = k * sampleRate / (2*(count-1))` |
| 幅度 | `float` dBFS，FFT / 窗函数 / 单边谱归一化在上游完成 |
| 显示 | −120～0 dBFS，EQ 另用 ±24 dB；不会把频谱 dBFS 当成 EQ 增益 |
| 无效值 | NaN/Inf 在绘图投影时转为 −120 dBFS |
| 刷新 | 默认 30 fps；中间帧覆盖，GUI 只消费最新帧 |
| 平滑 | 立即上升、48 dB/s 回落；超过 250 ms 无新帧后向底噪回落 |

若输入为归一化线性幅度，先转 `20*log10(max(amplitude, 1e-6))`；归一化功率使用 `10*log10(max(power, 1e-12))`。原始未归一化 FFT 数值不能直接当作 dBFS。当前接口不接受已经按对数频率排好的点，也不接收复数交错缓冲；这些需要在接入层转换或扩展帧格式。

共享频谱对象可比 QWidget 活得更久，不保留 QWidget 指针。提交和消费均使用 `try_lock`，复制到预分配数组，不为每帧分配堆内存；这是 FFT 工作线程接口，不宣称具备硬实时保证。

### 暂停和快速恢复频谱分析

内置 `PipeWireSpectrumSource` / `PcmFileSpectrumSource` 使用 `eq->spectrumInput()` 时，会自动跟随组件的频谱开关和显隐状态：`setSpectrumEnabled(false)`、隐藏或销毁控件会暂停分析；控件可见且频谱开启时恢复。无需在演示页面或业务页面另外连接暂停信号。控件创建后、首次显示前也处于暂停状态。

暂停期间保留采集连接、线程和缓冲，PCM 文件仍按原节奏读取；分析线程只维护最新 PCM 窗口，跳过加窗、FFT 和 dBFS 换算。恢复时处理当前积压的 PCM，并直接分析最新完整窗口，不等待下一个 hop 或重新攒满一窗。随后按原 `hopSize` 分析。暂停期间仍有采集和缓冲维护开销，并非完全停止音频线程。

邮箱会丢弃暂停前的待显示结果，并拒绝跨越暂停／恢复切换的旧 FFT 结果；已经开始的 FFT 可能完成，但不会作为恢复后的新帧发布。GUI 同时清空旧频谱和回落状态，并立即安排第一次刷新检查。若尚未采集到完整窗口，仍须等待足够的 PCM；如果恢复时已有窗口但超过 250 ms 没有新输入，不会重新显示这段过期数据。实际首帧延迟包含线程调度、FFT 和界面刷新，尚未测量板端耗时。

独立使用音频源、不连接 `EqWidget` 时，手动控制：

```cpp
source.setPaused(true);   // 保留采集及最新 PCM，暂停 FFT
source.setPaused(false);  // 从最新窗口恢复，不重新打开设备或文件
bool paused = source.isPaused();
```

两种音频源均提供上述接口。暂停时 `isRunning()` 仍可为 true；`stop()` 才会停止线程和采集。`setPaused()` 控制的是共享邮箱的分析状态，连接 `EqWidget` 后建议统一通过控件控制，避免手动控制与显隐切换相互覆盖。一个邮箱对应一个逻辑频谱源和一个消费方。

外部自有 FFT 不会自动停止计算。如需同样的暂停及旧结果保护，在开始一帧分析前获取状态，并在发布时传回该状态：

```cpp
const auto state = spectrumInput->analysisState();
if (state.enabled) {
    // 在工作线程中分析 PCM，生成 dbBins。
    spectrumInput->publish(dbBins, count, sampleRate, state);
}
```

普通三参数 `publish()` 仍可使用，但不能辨认调用前已经开始计算的旧结果。暂停状态切换接口供控制线程调用，不应在实时音频回调中调用。

## 滤波器和 DSP 一致性

滤波器包括 BYPASS、PEAK、HIGHSHELF1/2、LOWSHELF1/2、HIGHPASS1/2、LOWPASS1/2、BANDPASS、NOTCH、GAIN。一阶高通/低通渐近斜率为 6 dB/oct，二阶为 12 dB/oct；棚式滤波器在两侧趋向不同增益平台。Q 仅在有定义的类型上可调。滤波器参数范围为增益 ±24 dB、Q 0.1～30，频率严格小于 Nyquist。图形横轴为 20 Hz～min(20 kHz, 0.499 Fs)。

PEQ 和棚式系数遵循 Flutter DSSSP 的定义。不同 DSP 对 Q 和棚式转折频率可能有不同定义。为显示外部 DSP 的精确结果，`Band::useCustomCoefficients=true` 可提供归一化系数：

```text
H(z) = (b0 + b1*z^-1 + b2*z^-2) / (1 + a1*z^-1 + a2*z^-2)
```

这里命名对应 Flutter `(a0,a1,a2,b1,b2)` 的 `(b0,b1,b2,a1,a2)`。外部系数模式下控制点只读，避免参数变化后图形与实际系数不一致；参数化模式可以直接使用 `designBiquad()` 获取同定义系数。本组件不执行音频滤波和实时系数平滑。当前每段一个 biquad，没有实现任意阶级联。

## 性能设计和验证

- 网格与底部参数栏缓存为 RGB32；EQ 曲线和点缓存为预乘 ARGB32，透明图层只覆盖绘图区及控制点边缘。800×480、DPR=1 时这两张缓存约 2.53 MiB；演示中组件为 800×424，约 2.20 MiB。控制点的圆环和编号另缓存为 36×36 小图，32 段约增加 162 KiB。
- 底部图标、参数文字、Q 滑条分别比较自身依赖，只有变化时才重新绘制；修改未选中的 EQ 段不刷新底部。底部像素直接保存在不透明背景中，省去单独的透明层合成。EQ 图层用单一包围矩形跳过外围透明像素，避免细碎裁剪区域的管理开销。
- 每个 EQ 段缓存 dB 响应，仅修改段重算；汇总按“新段响应减旧段响应”增量更新，每 256 次增量更新后从各段缓存重新求和，限制浮点累积误差。频率对应的三角函数仅在尺寸、采样率、频率范围变化时计算。
- EQ 网格每逻辑像素最多两点，总上限 2048。绘制前用 O(N) 算法压缩折线，相对原始采样折线的纵向偏差不超过 0.2 个物理像素；不降低数值响应的采样密度。极窄高 Q 特征仍受屏幕及离散采样分辨率限制。
- FFT 映射最多 512 个对数显示点，以频带内峰值聚合，避免抽取丢掉高频尖峰。低频没有足够 FFT bin 时插值显示，不制造额外频率分辨率。
- 内置 PCM / PipeWire 分析器使用可复用的实数 FFT 计划：两个实数样本合并为一个复数，执行 N/2 点变换后恢复单边谱；窗函数、输入排列和旋转因子只在启动时准备。逐帧不重算旋转因子，不分配 FFT 临时内存，保持原有 Hann 窗、单边幅度归一化、FFT 尺寸和步进。
- 默认无频谱面积填充，频谱线不抗锯齿；EQ 线抗锯齿可关闭。隐藏组件停止定时器；频谱关闭且 EQ 静止时停止主动刷新。频谱开启但数据静止时按目标帧率轮询，仅变化时重绘。只有频谱变化时，按新旧曲线覆盖范围局部更新，避免重新合成坐标标签、底部控件及未变化的绘图区；面积填充开启时刷新范围包含到底噪基线的区域。
- 频率计算、频谱投影和位图准备在合并后的帧回调执行；`paintEvent` 主要做位图合成及频谱线绘制。首次显示/尺寸变化时允许重建静态背景。帧定时采用单调时钟的绝对截止时间，避免把 30 fps 每帧取整为 34 ms 后不断积累偏差。

用相同场景在设备上测量。显示基准默认使用预计算的确定性频谱，不启动 PCM 线程；因此它只回答“组件本身能否稳定绘制”。

```bash
./qdsssp_demo -platform linuxfb --benchmark 300
./qdsssp_demo -platform linuxfb --benchmark 300 --static-eq
./qdsssp_demo -platform linuxfb --benchmark 300 --no-aa
./qdsssp_demo -platform linuxfb --benchmark 300 --benchmark-pcm
./qdsssp_demo -platform linuxfb --benchmark 300 --benchmark-no-spectrum
./qdsssp_demo -platform linuxfb --benchmark 300 --benchmark-burst
```

基准场景固定为 32 段、8193 个频谱点、800×480 窗口，默认抗锯齿开启、频谱填充关闭。`--benchmark` 表示预热后生成指定次数的输入更新，**不是保证画出同样数量的帧**。基准运行时屏蔽触摸、鼠标和键盘输入，避免手工操作污染结果；普通演示仍可操作。默认先预热 60 次，可用 `--warmup` 修改；`--fps` 同时设置显示目标和演示输入定时器频率。

`--benchmark` 是显示层基准；`--benchmark-pcm` 使用实际 `DOGFIGHT.pcm`，把文件读取、FFT、频谱发布和组件绘制一起纳入测试；`--benchmark-no-spectrum` 测 EQ 绘制下限；`--benchmark-burst` 每个显示 tick 发布四个频谱帧，用于检查 latest-only 邮箱在突发输入下的行为。可用 `--pcm-path` 指定另一份 16-bit little-endian mono PCM。输出中的 `pcm_fft_mean_ms`、`pcm_fft_max_ms`、丢样本数和发布帧数用于判断音频线程是否成为瓶颈。

输出包含实际窗口/组件尺寸、统计时长、绘制次数/秒、进程 CPU、峰值 RSS，以及准备、响应计算、路径生成、图层绘制、底部参数栏、频谱、paintEvent、绘制起始间隔的平均/P50/P95/P99/最大值。`overlay` 包含 `curves` 和 `controls`（控制点及外部系数标注），底部单列为 `footer`；`prepare` 包含响应、路径、图层、底部及频谱，不能把这些嵌套行再次相加。只有实际执行的阶段才有样本；静态 EQ 预热后 `response_evaluations=0` 是预期结果。

- 耗时使用单调时钟，属于墙钟耗时；CPU 使用 `getrusage`，100% 表示占满一个核，不是整颗多核 CPU。
- 分位数最多保存每项前 16384 个样本，`sampled` 输出实际样本数；平均和最大值仍覆盖全部样本。超长测试建议分批比较。
- 合成显示基准的空间项在初始化预计算，每帧只做相位合成；`demo_source` 单独报告测试源开销。PCM 基准另报告文件读取、FFT 和发布统计，组件仍只负责显示外部 FFT 数据。
- 统计末尾含 100 ms 排空时间，使最后一次输入有机会绘制；短测试的绘制吞吐会略低。
- 普通使用不启用分项采样，不分配诊断样本缓冲；需要时调用 `startProfiling()` / `stopProfiling()` 并读取 `profile()`。

目前默认保持 **30 fps**。`paint_fps` 是软件绘制吞吐；组件计时不包含 Qt 平台提交、显示扫描或触控硬件延迟，不能当作屏幕实际刷新率或触摸到显示延迟。

测试覆盖全部类型数值边界、独立复数频响验证、长期增量求和、折线误差上限、缓存图像状态一致性、外部系数、频谱峰值、并发帧一致性、鼠标和 Q 滑条、双指/十指状态切换、取消、图标资源和静止不重绘。
实数 FFT 另用独立双精度 DFT 验证 256～16384 点变换、环形窗口起点、DC / Nyquist / 单频幅度和静音底噪。频谱局部更新逐像素检查新旧图像差异均在实际刷新范围内，覆盖填充、回落、关闭、恢复和 1× / 1.5× / 2× 缩放。2026-10-02 的优化说明与对比结果见 [性能记录](docs/performance-2026-10-02.md)。

基准程序通过 CMake 构建，输出包含响应计算、准备、绘制和 CPU 统计；测试结果不作为组件运行时依赖提交。

## 参考来源

参考本机 `dsssp_flutter` 的滤波器定义、触摸交互及图标；原始图标字体和相关许可保留在 `assets/fonts/`。参考 `ZLEqualizer` 的计算/绘制分离及缓存思路，未引入 JUCE 或其源码。`THIRD_PARTY_NOTICES.md` 记录复用来源。
