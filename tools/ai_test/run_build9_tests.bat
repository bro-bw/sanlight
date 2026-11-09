@echo off
setlocal
call "%~dp0disk_cache_test.bat" || exit /b 1
call "%~dp0surface_memory_compare.bat" || exit /b 1
call "%~dp0checkpoint_test.bat" || exit /b 1
call "%~dp0queue_host.bat" || exit /b 1
call "%~dp0temporal_host.bat" || exit /b 1
call "%~dp0ai_cache_test.bat" || exit /b 1
call "%~dp0..\..\build-v1.3.bat" || exit /b 1
echo All build9 regression tests and production build passed.
exit /b 0
