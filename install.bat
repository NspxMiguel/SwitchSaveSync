@echo off
rem SwitchSaveSync — double-click this and it puts the app on your SD card.
rem
rem This is the "exe" of the project: a file you click, that asks which card
rem and copies the files there. It does nothing by itself — it starts
rem install.ps1, which is the actual installer and is right next to this file
rem (or comes from GitHub, if you only downloaded this one).
rem
rem Nothing here needs administrator. Nothing here formats or deletes.

setlocal
title SwitchSaveSync

if exist "%~dp0install.ps1" (
    powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" %*
) else (
    echo Baixando o instalador... / Downloading the installer...
    powershell -NoProfile -ExecutionPolicy Bypass -Command "[Net.ServicePointManager]::SecurityProtocol=[Net.SecurityProtocolType]::Tls12; irm https://raw.githubusercontent.com/NspxMiguel/SwitchSaveSync/main/install.ps1 | iex"
)

set RC=%ERRORLEVEL%

rem Sem a pausa, a janela fecha antes de a pessoa ler o que aconteceu. Na
rem integracao continua nao tem ninguem pra apertar tecla: SSS_SEM_PAUSA=1.
echo.
if not "%SSS_SEM_PAUSA%"=="1" pause
exit /b %RC%
