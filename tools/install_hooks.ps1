# Installs the pre-commit content guard into .git/hooks (run once per clone).
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$hook = Join-Path $root ".git\hooks\pre-commit"
$py = (Get-Command python -ErrorAction SilentlyContinue).Source
if (-not $py) { throw "python not found on PATH" }
$body = @"
#!/bin/sh
# Installed by tools/install_hooks.ps1 - rejects game content and generated code.
exec "$py" "$($root -replace '\\','/')/tools/check_content.py" --staged
"@
Set-Content -Path $hook -Value $body -NoNewline -Encoding ascii
Write-Host "installed $hook"
