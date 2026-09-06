---
name: build-esp-idf-alarm
description: 'Build the esp32_wifi_alarm_idf project on Windows using the installed ESP-IDF PowerShell initialization flow. Use when asked to compile, rebuild, or prepare flash artifacts for the ESP-IDF alarm Wi-Fi project.'
argument-hint: 'Describe whether you want build only, flash, monitor, or a specific COM port.'
user-invocable: true
disable-model-invocation: false
---

# Build the ESP-IDF alarm project

Use this skill when the task is to build or rebuild `esp32_wifi_alarm_idf` in this repository.

## Project location

- Project directory: `esp32_wifi_alarm_idf`
- Default target: `esp32`
- Preferred shell: PowerShell on Windows

## Required initialization

Do not assume a plain PowerShell session already has ESP-IDF configured.

Prefer the same initialization path as the installed ESP-IDF PowerShell shortcut:

```powershell
. 'C:\Espressif\Initialize-Idf.ps1' -IdfId 'esp-idf-ab7213b7273352b64422b1f400ff27a0'
```

If that script is missing, fall back to the installed ESP-IDF v5.3.5
PowerShell profile:

```powershell
. 'C:\Espressif\tools\Microsoft.v5.3.5.PowerShell_profile.ps1'
```

The equivalent standalone shortcut command is:

```powershell
powershell.exe -NoExit -ExecutionPolicy Bypass -NoProfile -Command "& {. 'C:\Espressif\tools\Microsoft.v5.3.5.PowerShell_profile.ps1' }"
```

Then move into the project directory and run the requested command.

## Standard commands

Build:

```powershell
Set-Location 'C:\Users\richard\gitroot\zharduino\esp32_wifi_alarm_idf'
idf.py build
```

Set target and build:

```powershell
Set-Location 'C:\Users\richard\gitroot\zharduino\esp32_wifi_alarm_idf'
idf.py set-target esp32
idf.py build
```

Flash:

```powershell
Set-Location 'C:\Users\richard\gitroot\zharduino\esp32_wifi_alarm_idf'
idf.py -p COM5 flash
```

Flash and monitor:

```powershell
Set-Location 'C:\Users\richard\gitroot\zharduino\esp32_wifi_alarm_idf'
idf.py -p COM5 flash monitor
```

## Expected outputs

- Build folder: `esp32_wifi_alarm_idf\build\`
- App binary: `esp32_wifi_alarm_idf\build\esp32_wifi_alarm_idf.bin`
- ELF: `esp32_wifi_alarm_idf\build\esp32_wifi_alarm_idf.elf`

## Notes

- `sdkconfig.defaults` should remain ASCII-only on Windows so `kconfgen` does not fail with a GBK decode error.
- The generated `esp32_wifi_alarm_idf\build\` folder is intentionally gitignored.
- If the environment is already initialized by the ESP-IDF shortcut, only `Set-Location` plus the `idf.py` command is needed.
