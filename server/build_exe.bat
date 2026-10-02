@echo off
rem Builds dist\CXLRemoteServer.exe (single file, no console). Needs: pip install pyinstaller
cd /d "%~dp0"
python -m PyInstaller --noconfirm --clean --onefile --windowed --name CXLRemoteServer ^
  --icon icon.ico --add-data "icon.ico;." --collect-data soundcard cxl_remote_gui.pyw
