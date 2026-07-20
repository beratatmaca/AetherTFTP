#Requires -RunAsAdministrator
# AetherTFTP Windows Service Uninstallation Script
# Run as Administrator in PowerShell

Stop-Service -Name "AetherTFTP" -ErrorAction SilentlyContinue
Remove-Service -Name "AetherTFTP" -ErrorAction SilentlyContinue

Write-Host "AetherTFTP service uninstalled successfully." -ForegroundColor Green
