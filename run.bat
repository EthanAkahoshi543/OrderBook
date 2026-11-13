@echo off
set NAME=%~1
if "%NAME%"=="" set NAME=main
call "H:\VisualStudiosDownloadFodler\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
cd /d "%~dp0"
cl /nologo /std:c++20 /EHsc /W4 /O2 %NAME%.cpp /Fe:%NAME%.exe || exit /b 1
"%~dp0%NAME%.exe"
