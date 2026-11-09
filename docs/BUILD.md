# 构建依赖

## 环境变量

| 变量 | 含义 | 未设置时 |
| --- | --- | --- |
| `AE_SDK_ROOT` | Adobe After Effects SDK 根目录 | `deps/AfterEffectsSDK` |
| `ONNXRUNTIME_INCLUDE` | 包含 `onnxruntime_cxx_api.h` 的目录 | `deps/onnxruntime/include` |
| `OPENCV_FLAGS_FILE` | 设置 `CVINC` 和 `CVLIBS` 的 `.cmd` 文件 | `deps/native-motion-deps/flags.cmd` |
| `VCVARS64` | 可选的 MSVC x64 环境脚本 | 使用当前环境或 `vswhere.exe` |

路径使用绝对路径，构建会进入 `build/`。`OPENCV_FLAGS_FILE` 是本机可执行配置，只使用自己信任的文件。

## OpenCV

当前源使用 OpenCV 5.0.0，原依赖归档 SHA-256 保存在 `notices/OpenCV/SOURCE.txt`。不把已编译静态库或第三方源码复制进本仓库。原构建选择：

```text
CMAKE_BUILD_TYPE=Release
BUILD_LIST=core,imgproc,video
BUILD_SHARED_LIBS=OFF
BUILD_WITH_STATIC_CRT=ON
BUILD_TESTS=OFF
BUILD_PERF_TESTS=OFF
BUILD_EXAMPLES=OFF
BUILD_opencv_apps=OFF
BUILD_JAVA=OFF
BUILD_opencv_python3=OFF
BUILD_opencv_python2=OFF
WITH_IPP=OFF
WITH_OPENCL=OFF
WITH_ITT=OFF
WITH_TBB=OFF
WITH_OPENMP=OFF
WITH_PTHREADS_PF=OFF
WITH_DNN=OFF
OPENCV_DNN_OPENCL=OFF
CPU_DISPATCH=SSE4_1,AVX2
```

`flags.cmd` 的示例适用于源码放在 `opencv-5.0.0/`、构建产物放在同级 `build/` 的布局；如果安装布局不同，需要相应修改：

```bat
@echo off
set "CVROOT=%~dp0"
set "CVINC=/I "%CVROOT%build" /I "%CVROOT%opencv-5.0.0\modules\core\include" /I "%CVROOT%opencv-5.0.0\modules\imgproc\include" /I "%CVROOT%opencv-5.0.0\modules\video\include""
set "CVLIBS="%CVROOT%build\lib\opencv_video500.lib" "%CVROOT%build\lib\opencv_imgproc500.lib" "%CVROOT%build\lib\opencv_geometry500.lib" "%CVROOT%build\lib\opencv_flann500.lib" "%CVROOT%build\lib\opencv_core500.lib" "%CVROOT%build\3rdparty\lib\zlib.lib" "%CVROOT%build\3rdparty\lib\libclapack.lib" ole32.lib"
```

## 运行时

插件 CPU 路径从与 AEX 同级的 `SanLight_AI/` 读取 `onnxruntime.dll` 和 `depth_anything.onnx`。GPU 候选还按源码中的检查选择匹配的 AE DirectML 运行库；完整 GPU 运行条件须在目标 AE 安装中验证。不要把任意不同版本的 DLL 直接混用。

本仓库不随附、自动下载或上传模型、SDK 和运行库。开发时沿用已有许可来源和匹配版本；分发二进制包时保留其许可和来源记录，并记录文件 SHA-256。
