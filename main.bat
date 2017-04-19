@if [%1]==[] goto usage

@call g:\dev\boost_src\fetch.bat %1
main g:\dev\boost_src\ %1
@goto :eof

:usage
@echo Usage: main version
@echo e.g. : main 1.64.0
@exit /B 1
