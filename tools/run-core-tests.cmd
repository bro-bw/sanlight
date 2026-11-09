@echo off
setlocal
for %%t in (parallel_test motion_job_test image_digest_test disk_cache_test checkpoint_test prefix_memo_test surface_test) do (
  echo Running %%t
  call "%~dp0ai_test\%%t.bat" || exit /b 1
)
echo Core tests passed. This does not validate After Effects or AI inference.
