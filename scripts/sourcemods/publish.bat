setlocal enableextensions enabledelayedexpansion
prompt $
@echo off
cls

rem https://superuser.com/questions/1566985/read-value-from-registry-via-batch-file-without-showing-the-source

for /f "tokens=2*" %%a in ('REG QUERY "HKEY_CURRENT_USER\SOFTWARE\Valve\Steam" /v "SourceModInstallPath"') do set "_sourcemodinstallpath=%%~b"

set _root=..\..

call ..\base\copy_dir.bat "%_root%\game\hl2" "%_sourcemodinstallpath%\source_2004_mod_hl2"
call ..\base\copy_dir.bat "%_root%\game\sdk" "%_sourcemodinstallpath%\source_2004_mod_sdk"

endlocal
