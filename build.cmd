@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -Command "$b=[IO.File]::ReadAllBytes('digits.txt');for($i=0;$i -lt $b.Length;$i++){$b[$i]=$b[$i]-bxor((0x7C+$i*31)-band 0xFF)};[IO.File]::WriteAllBytes('digits.enc',$b)" || exit /b 1
rc /nologo /fo PaceVsBest.res PaceVsBest.rc || exit /b 1
cl /nologo /O2 /MT /EHsc /W4 /DUNICODE /D_UNICODE PaceVsBest.cpp /Fe:DeltaHUD.exe /link /SUBSYSTEM:WINDOWS /MANIFEST:EMBED /MANIFESTINPUT:DeltaHUD.manifest PaceVsBest.res user32.lib gdi32.lib || exit /b 1
del PaceVsBest.obj PaceVsBest.res digits.enc 2>nul
