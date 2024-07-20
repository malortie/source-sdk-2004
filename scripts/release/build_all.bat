setlocal enableextensions enabledelayedexpansion
prompt $
echo off
cls

set _version=1.0.0
set _platform_RID=win-x86

call build.bat "..\.." %_version% %_platform_RID% game\hl2 source_2004_mod_hl2
call build.bat "..\.." %_version% %_platform_RID% game\sdk source_2004_mod_sdk

endlocal
