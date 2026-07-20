#Requires -RunAsAdministrator
# AetherTFTP Windows Service Installation Script
# Run as Administrator in PowerShell

$binPath = Join-Path $PSScriptRoot "aethertftp.exe"
if (-not (Test-Path $binPath)) {
    $binPath = Join-Path (Get-Location) "aethertftp.exe"
}

if (-not (Test-Path $binPath)) {
    Write-Error "aethertftp.exe could not be found. Please place this script in the same directory as the executable."
    exit 1
}

New-Service -Name "AetherTFTP" `
            -BinaryPathName "`"$binPath`" --service" `
            -DisplayName "AetherTFTP Server" `
            -Description "Modern lightweight TFTP client and server background service." `
            -StartupType Automatic

Write-Host "AetherTFTP service installed successfully." -ForegroundColor Green
Write-Host "Start the service using: Start-Service AetherTFTP" -ForegroundColor Green
