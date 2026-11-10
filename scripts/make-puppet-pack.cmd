@echo off
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0make-puppet-pack.ps1" %*
