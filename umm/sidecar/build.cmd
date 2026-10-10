@echo off
rem ============================================================
rem  build.cmd - compile the bridge sidecar -> AdofPerfectUmm.dll
rem  (official UMM kernel + this sidecar = the tool's default install)
rem ============================================================
setlocal enabledelayedexpansion

set "CSC=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\Roslyn\csc.exe"
if not exist "%CSC%" set "CSC=C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\Roslyn\csc.exe"
if not exist "%CSC%" (
  for /f "delims=" %%i in ('where csc.exe 2^>nul') do set "CSC=%%i"
)
if not exist "%CSC%" (
  echo [x] csc.exe not found
  exit /b 1
)

set "MANAGED=%~1"
if "%MANAGED%"=="" set "MANAGED=%ADOFAI_MANAGED%"
if "%MANAGED%"=="" set "MANAGED=D:\Program Files (x86)\Steam\steamapps\common\A Dance of Fire and Ice\A Dance of Fire and Ice_Data\Managed"
if not exist "%MANAGED%\mscorlib.dll" (
  echo [x] Managed folder not found: "%MANAGED%"
  exit /b 1
)

set "HERE=%~dp0"
set "OUT=%HERE%..\AdofPerfectUmm.dll"
set "RSP=%TEMP%\adofperfect_sidecar.rsp"

> "%RSP%" echo /nologo
>>"%RSP%" echo /noconfig
>>"%RSP%" echo /nostdlib+
>>"%RSP%" echo /target:library
>>"%RSP%" echo /optimize+
>>"%RSP%" echo /platform:anycpu
>>"%RSP%" echo /langversion:7.3
>>"%RSP%" echo /out:"%OUT%"
>>"%RSP%" echo /reference:"%MANAGED%\mscorlib.dll"
>>"%RSP%" echo /reference:"%MANAGED%\netstandard.dll"
>>"%RSP%" echo /reference:"%MANAGED%\System.dll"
>>"%RSP%" echo /reference:"%MANAGED%\System.Core.dll"
>>"%RSP%" echo /reference:"%MANAGED%\System.Xml.dll"
for %%f in ("%HERE%src\*.cs") do >>"%RSP%" echo "%%f"

"%CSC%" /noconfig @"%RSP%"
if errorlevel 1 (
  echo [x] build failed
  exit /b 1
)
echo [ok] %OUT%
endlocal
