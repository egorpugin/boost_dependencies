@if [%1]==[] goto usage

fetch.bat %1
main.exe -v %1

@goto :eof

:usage
@echo Usage: fetch.bat version
@echo e.g. : fetch.bat 1.64.0
@exit /B 1
