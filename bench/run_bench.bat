@echo off
rem LLM 預測模型測試（參數見 README.md）
chcp 65001 >nul
set PYTHONIOENCODING=utf-8
"C:\Users\zex55\src\llama-b11177\venv\Scripts\python.exe" "%~dp0bench.py" %*
