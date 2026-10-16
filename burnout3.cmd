@echo off
rem burnout3: the project CLI (burnout3.py). Python 3.9 or newer.
where py >nul 2>nul
if %errorlevel%==0 (
    py -3 "%~dp0burnout3.py" %*
) else (
    python "%~dp0burnout3.py" %*
)
exit /b %errorlevel%
