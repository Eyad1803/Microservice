@echo off
setlocal
echo Checking TCP port 8000...
netstat -ano | findstr /R /C:":8000 .*LISTENING"
if errorlevel 1 (
  echo Port 8000 is free.
) else (
  echo The final column above is the listener PID.
)
