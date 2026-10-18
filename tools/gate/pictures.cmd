@echo off
rem Picture step of the merge gate (lane C's check): tools\gate\pictures.cmd <build bin folder>
rem Reference captures replayed exact + GPU, identical to their reference pictures. The captures are game pictures
rem made from your own disc, so they are not in the repository: DOTR_PICTURE_REFS = a folder with check-pictures.cmd
rem (and its captures); the maintainers keep theirs in ..\..\..\dotr-big\chatC\port\refs.
rem Exit 0 OK, 1 a capture differs (one line each), 3 the replay tool is missing, 4 no references (skipped).
if "%DOTR_PICTURE_REFS%"=="" set DOTR_PICTURE_REFS=%~dp0..\..\..\dotr-big\chatC\port\refs
if not exist "%DOTR_PICTURE_REFS%\check-pictures.cmd" exit /b 4
call "%DOTR_PICTURE_REFS%\check-pictures.cmd" %1
exit /b %errorlevel%
