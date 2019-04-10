@if [%1]==[] goto usage

@setlocal
@cd d:\dev\boost

@set j=20

git fetch --recurse-submodules -j%j%
@if %errorlevel% neq 0 exit /b %errorlevel%

@git rev-parse boost-%1 > nul 2> nul
@if %errorlevel% neq 0 goto :master

git checkout tags/boost-%1 -b %1
::@if %errorlevel% neq 0 exit /b %errorlevel%
@set tree=boost-%1
@goto :next

:master
git checkout master
git reset --hard
git pull origin master
@set tree=master

:next
git submodule update --init --recursive -j %j%
@if %errorlevel% neq 0 exit /b %errorlevel%

git ls-tree -r -d %tree% | grep libs/ | grep -v doc | grep -v headers | gawk '{print $4" "$3}' | grep -v "numeric " > d:\dev\boost_deps\%1.commits
@if %errorlevel% neq 0 exit /b %errorlevel%

@goto :eof

:usage
@echo Usage: fetch.bat version
@echo e.g. : fetch.bat 1.64.0
@exit /B 1
