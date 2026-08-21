@echo off
setlocal
cd /d "%~dp0"

if not exist ".venv\Scripts\python.exe" (
  echo ERROR: SmartOfficeBackend virtual environment was not found.
  echo Expected: %CD%\.venv\Scripts\python.exe
  exit /b 1
)

netstat -ano | findstr /R /C:":8000 .*LISTENING" >nul
if not errorlevel 1 (
  echo Port 8000 is already in use. No second backend was started.
  echo Run check_backend.cmd to see the current listener.
  exit /b 1
)

echo Starting Smart Office Backend from:
echo %CD%
echo.
echo Press Ctrl+C to stop the server.
set "SMART_OFFICE_DATABASE_PATH="
".venv\Scripts\python.exe" -m uvicorn app.main:app --host 0.0.0.0 --port 8000
