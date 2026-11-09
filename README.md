# SanLight

Windows x64 / Adobe After Effects 光照插件源码，包含本机深度推理、相邻帧运动估计、时间稳定和表面缓存。

当前主线是 **v1.3 build 23 候选版**（PiPL/代码版本 `624151`）。这是源码维护仓库；不包含 Adobe SDK、模型、运行库、插件二进制或用户素材。

| 版本 | 状态 |
| --- | --- |
| build 23 | 最新候选源码；已通过历史独立 SDK 宿主检查，尚未进行此版实际 AE 预览／导出验证 |
| build 22 | 最近已打包候选；也是 build 23 的输出对照基线 |
| build 15 | 最后经过实际 AE 验证的回退版本；旧安装包在本机保留 |

build 23 只减少未启用的光照计算，不解决既有运动“果冻”及遮挡问题。不应把它标为稳定发布或把独立宿主耗时称为 AE 实测。

## 构建

需要 Windows x64、MSVC C++ 工具和 Windows SDK；完整构建另需自行准备：

- After Effects SDK（含 `Examples/Headers`、`Examples/Util` 和 `PiPLtool.exe`）。
- 与运行时兼容的 ONNX Runtime C/C++ 头文件。
- OpenCV 5.0.0 的静态 Release、静态 CRT（`/MT`）构建：core、imgproc、video 及其传递依赖。原构建参数见 [依赖说明](docs/BUILD.md)。

在 x64 Native Tools Command Prompt 中设置本机路径，然后构建：

```bat
set "AE_SDK_ROOT=C:\deps\AfterEffectsSDK"
set "ONNXRUNTIME_INCLUDE=C:\deps\onnxruntime\include"
set "OPENCV_FLAGS_FILE=C:\deps\native-motion-deps\flags.cmd"
build-v1.3.bat gpu
```

这些是示例路径。也可将依赖放入仓库内被忽略的 `deps/`；默认布局见构建脚本。若不使用开发者命令行，可设置 `VCVARS64` 指向自己的 `vcvars64.bat`，或让脚本通过 Visual Studio Installer 的 `vswhere.exe` 检测安装位置。

GPU 候选输出为 `build/gpu/SanLight_v1_3.aex`；不传 `gpu` 时输出 CPU 构建到 `build/SanLight_v1_3.aex`。脚本完成后自动验证 PiPL 资源，不会安装或替换 AE 中的插件。运行插件还需要匹配的本机模型和运行库，见 [依赖说明](docs/BUILD.md)。

## 检查

```bat
tools\run-core-tests.cmd
```

此命令运行七组已有 C++ 回归：并行执行、运动任务、内容摘要、磁盘缓存、检查点、前缀缓存、表面时间稳定。它们不需要 Adobe SDK、模型或运行中的 AE。推送和拉取请求会运行同一组 GitHub Actions 检查。

其余 `tools/ai_test/` 文件保留为历史诊断工具；部分要求 SDK、素材、模型或旧版源码，不能当作全部可独立运行的测试集。详见 [验证记录](docs/VALIDATION.md)。

## 维护与许可

维护步骤与候选转发布条件见 [MAINTENANCE.md](MAINTENANCE.md)，本次整理见 [CHANGELOG.md](CHANGELOG.md)。

本次整理未为 SanLight 自有源码新增开源许可证。`notices/` 是从既有 build 22 安装包逐文件保留的第三方许可，其中 `notices/LICENSE` 属于 Microsoft，不是 SanLight 整仓许可。Adobe SDK、模型和运行库应分别按其许可获取和使用。
