@echo off
chcp 65001 >nul
setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0"

if not defined PYTHON (
  for %%P in (
    "C:\Program Files\Python314\python.exe"
    "C:\Program Files\Python313\python.exe"
    "C:\Program Files\Python312\python.exe"
    "C:\Program Files\Python311\python.exe"
    "%LocalAppData%\Programs\Python\Python314\python.exe"
    "%LocalAppData%\Programs\Python\Python313\python.exe"
    "%LocalAppData%\Programs\Python\Python312\python.exe"
    "%LocalAppData%\Programs\Python\Python311\python.exe"
    "C:\msys64\mingw64\bin\python.exe"
  ) do if not defined PYTHON if exist %%P set "PYTHON=%%~P"
  for %%P in (python.exe py.exe) do if not defined PYTHON for /f "delims=" %%I in ('where %%P 2^>nul') do if not defined PYTHON set "PYTHON=%%I"
)
if not defined PYTHON (
  echo [ERRO] Python 3.10 ou mais recente nao foi encontrado.>&2
  exit /b 1
)

if not defined GOW3R_DATA_DIR set "GOW3R_DATA_DIR=%CD%"
if not defined GOW3R_GAME_DIR set "GOW3R_GAME_DIR=%~1"
if not defined GOW3R_GAME_DIR set "GOW3R_GAME_DIR=..\CUSA01623 GoW3Remaster 1.02"
if not defined GOW3R_USER_DIR set "GOW3R_USER_DIR=%GOW3R_DATA_DIR%\user\CUSA01623"
if not defined GOW3R_CONFIG set "GOW3R_CONFIG=%GOW3R_DATA_DIR%\gow3r.ini"
if not defined GOW3R_TIMEOUT set "GOW3R_TIMEOUT=0"
if not defined GOW3R_READBACKS set "GOW3R_READBACKS=0"
if not defined GOW3R_READBACK_LINEAR set "GOW3R_READBACK_LINEAR=1"
if not defined GOW3R_ALIAS_SYNC set "GOW3R_ALIAS_SYNC=1"
if not defined BB_ASYNC_PIPELINES set "BB_ASYNC_PIPELINES=0"
if not defined GOW3R_DMEM_MB set "GOW3R_DMEM_MB=7104"
if not defined GOW3R_VBLANK_HZ set "GOW3R_VBLANK_HZ=60"
if not defined GOW3R_INTERNAL_WIDTH set "GOW3R_INTERNAL_WIDTH=1920"
if not defined GOW3R_INTERNAL_HEIGHT set "GOW3R_INTERNAL_HEIGHT=1080"
if not defined GOW3R_OVERLAY set "GOW3R_OVERLAY=0"
if not defined GOW3R_FULLSCREEN set "GOW3R_FULLSCREEN=0"
if not defined GOW3R_PATCH_SKIP_INTRO set "GOW3R_PATCH_SKIP_INTRO=0"
if not defined GOW3R_PATCH_SKIP_VIDEO set "GOW3R_PATCH_SKIP_VIDEO=0"

set "GAME=%GOW3R_GAME_DIR%"
set "OUT=%GOW3R_DATA_DIR%\out"
set "LOG=%GOW3R_DATA_DIR%\gow3r.log"
if not exist "%GAME%\eboot.bin" (
  echo [ERRO] eboot.bin nao encontrado em "%GAME%".>&2
  exit /b 1
)
if not exist "%OUT%" mkdir "%OUT%"
if not exist "%GOW3R_USER_DIR%" mkdir "%GOW3R_USER_DIR%"

"%PYTHON%" scripts\game_check.py "%GAME%"
if errorlevel 1 exit /b %ERRORLEVEL%

set "CACHE_KEY_TMP=%OUT%\cache.key.tmp"
"%PYTHON%" scripts\cache_key.py "%GAME%" > "%CACHE_KEY_TMP%"
if errorlevel 1 exit /b !ERRORLEVEL!
set /p "CACHE_KEY="<"%CACHE_KEY_TMP%"
if not defined CACHE_KEY (
  echo [ERRO] Nao foi possivel calcular a chave do cache.>&2
  exit /b 2
)
set "REBUILD=1"
if exist "%OUT%\boot-linked.bin" if exist "%OUT%\cache.key" (
  set /p OLD_KEY=<"%OUT%\cache.key"
  if "!OLD_KEY!"=="!CACHE_KEY!" set "REBUILD=0"
)

if "!REBUILD!"=="1" (
  echo Preparando imagem GOW3IMG1...
  "%PYTHON%" scripts\prepare.py "%GAME%" --out "%OUT%" || exit /b !ERRORLEVEL!
  "%PYTHON%" scripts\link_modules.py "%GAME%" --out "%OUT%" || exit /b !ERRORLEVEL!
  "%PYTHON%" scripts\content_profile.py "%GAME%" --out "%OUT%" --sku full || exit /b !ERRORLEVEL!
  move /y "%CACHE_KEY_TMP%" "%OUT%\cache.key" >nul
) else (
  echo Cache GOW3IMG1 valido: !CACHE_KEY!
  del /q "%CACHE_KEY_TMP%" >nul 2>nul
)

"%PYTHON%" scripts\import_coverage.py "%OUT%\link.json" --out "%OUT%\coverage.json" --strict
if errorlevel 1 (
  echo [ERRO] Existem imports host sem contrato. Consulte "%OUT%\coverage.json".>&2
  exit /b 3
)

set "PATCH_NAMES="
if "%GOW3R_PATCH_SKIP_INTRO%"=="1" set "PATCH_NAMES=Skip Intro"
if "%GOW3R_PATCH_SKIP_VIDEO%"=="1" (
  if defined PATCH_NAMES (
    set "PATCH_NAMES=!PATCH_NAMES!;Skip Any Video With X Button"
  ) else (
    set "PATCH_NAMES=Skip Any Video With X Button"
  )
)
"%PYTHON%" scripts\patches.py --xml "patches\God_of_War_III_Remastered.xml" --app-version 01.02 --extra "!PATCH_NAMES!" --out "%OUT%"
if errorlevel 1 (
  echo [ERRO] Nao foi possivel preparar os patches selecionados.>&2
  exit /b 4
)

if not exist "out\gow3r-port.exe" call build.bat || exit /b !ERRORLEVEL!

echo Iniciando God of War III Remastered. Log: "%LOG%"
"out\gow3r-port.exe" "%OUT%\boot-linked.bin" --content-profile "%OUT%\content.bin" --app0 "%GAME%" --user "%GOW3R_USER_DIR%" --patches "%OUT%\patches.bin" --timeout "%GOW3R_TIMEOUT%" %2 %3 %4 %5 %6 %7 %8 %9 >>"%LOG%" 2>&1
set "RESULT=!ERRORLEVEL!"
echo Processo encerrado com codigo !RESULT!.
exit /b !RESULT!
