# 验证范围

## 2026-11-09 源码整理验证

- 27 个生产 `src/` 文件逐文件 SHA-256 对照原 build 23 候选；生产源码未修改。
- 使用本机已有外部 SDK、ONNX Runtime 头文件和 OpenCV 静态库，成功重建 GPU AEX；自动检查通过 12 项 PiPL 属性。没有向 AE 安装此产物。
- 七组已有回归通过：`parallel_test`、`motion_job_test`、`image_digest_test`（含摘要失败回退）、`disk_cache_test`、`checkpoint_test`、`prefix_memo_test`、`surface_test`。
- 未重新执行完整模型推理、真实 AEX 输出对照或 AE 预览／导出；先前结果作为历史记录保留。GitHub Actions 是否运行成功应以仓库实际运行结果为准。

## 历史工具限制

`track_compare` 引用 build 5 源码且要求外部输入序列；首次整理时确认缺少旧源，未加入基础检查。`surface_memory_compare`、`ai_cache_test`、`sequence_loop_build5` 等也依赖旧版本；不要把历史聚合脚本 `run_build9_tests.bat` 当作当前版本的完整检查入口。历史工具的存在不表示这些旧源码或数据已上传。

SDK 宿主工具需要外部 Adobe SDK，推理测试需要本机运行库和模型。Python 预览工具还需要 NumPy、Pillow 以及输入文件。使用这些工具前阅读参数和具体依赖，并把输出放在被忽略的 `build/` 或独立工作目录。

## 发布前尚需验证

build 23 尚未进行此版真实 AE 验证；build 15 是最后经过实际 AE 验证的版本。build 22 为最近已打包候选，build 23 为最新源码候选。历史独立宿主结果与基础 CI 都不能替代正式发布前在目标 AE 中进行的实际使用检查。
