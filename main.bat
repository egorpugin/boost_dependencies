@if [%1]==[] goto usage

@pushd d:\dev\boost
@call d:\dev\boost\fetch.bat %1
@popd
@if %errorlevel% neq 0 exit /b %errorlevel%

main d:\dev\boost %1
@goto :eof

:usage
@echo Usage: main.bat version
@echo e.g. : main.bat 1.64.0
@exit /B 1
