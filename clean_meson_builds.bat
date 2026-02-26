@echo off
setlocal

set ROOT=%~dp0
pushd "%ROOT%" >nul

echo Cleaning Meson build directories in %CD%

for /d %%D in (build-cross*) do (
  echo [DEL] %%D
  rmdir /s /q "%%D"
)

if exist build-msvc (
  echo [DEL] build-msvc
  rmdir /s /q build-msvc
)

if exist build-mingw (
  echo [DEL] build-mingw
  rmdir /s /q build-mingw
)

if exist build (
  echo [DEL] build
  rmdir /s /q build
)

popd >nul
echo Done.
exit /b 0
