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
  echo ERROR: Port 8000 is already in use. No simulation backend was started.
  echo Run check_backend.cmd to inspect the current listener.
  exit /b 1
)

".venv\Scripts\python.exe" tools\prepare_phase3_simulation.py
if errorlevel 1 exit /b 1

set "SMART_OFFICE_DATABASE_PATH=%CD%\smart_office.phase3_simulation.db"
echo Starting Smart Office Phase 3 simulation backend with:
echo %SMART_OFFICE_DATABASE_PATH%
echo.
echo Press Ctrl+C to stop the server.
".venv\Scripts\python.exe" -m uvicorn app.main:app --host 0.0.0.0 --port 8000 --workers 1
