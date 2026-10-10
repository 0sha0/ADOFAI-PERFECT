@echo off
rem ============================================================
rem  build.cmd - compile our own UMM-compatible loader -> UnityModManager.dll
rem
rem  usage: build.cmd [<game Managed folder>]
rem  default: Steam path, or env var ADOFAI_MANAGED
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
if not exist "%MANAGED%\UnityEngine.CoreModule.dll" (
  echo [x] Managed folder not found: "%MANAGED%"
  exit /b 1
)
if not exist "%MANAGED%\mscorlib.dll" (
  echo [x] game BCL not found in "%MANAGED%"
  exit /b 1
)

set "HERE=%~dp0"
set "OUT=%HERE%UnityModManager.dll"
set "RSP=%TEMP%\adofperfect_umm.rsp"

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
>>"%RSP%" echo /reference:"%MANAGED%\System.Runtime.Serialization.dll"
>>"%RSP%" echo /reference:"%MANAGED%\UnityEngine.CoreModule.dll"
>>"%RSP%" echo /reference:"%MANAGED%\UnityEngine.IMGUIModule.dll"
>>"%RSP%" echo /reference:"%MANAGED%\UnityEngine.TextRenderingModule.dll"
>>"%RSP%" echo /reference:"%MANAGED%\UnityEngine.InputLegacyModule.dll"
>>"%RSP%" echo /reference:"%MANAGED%\0Harmony.dll"
for %%f in ("%HERE%src\*.cs") do >>"%RSP%" echo "%%f"

"%CSC%" /noconfig @"%RSP%"
if errorlevel 1 (
  echo [x] build failed
  exit /b 1
)
echo [ok] %OUT%
endlocal


