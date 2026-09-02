# Windows Scheduled Task: start the client at boot and restart it if it exits.
#   powershell -ExecutionPolicy Bypass -File deploy\register-windows-task.ps1
$InstallDir = "C:\data-sync"
$Action = New-ScheduledTaskAction -Execute "$InstallDir\sync_client.exe" -Argument "$InstallDir\clientConfig.json" -WorkingDirectory $InstallDir
$Trigger = New-ScheduledTaskTrigger -AtStartup
$Settings = New-ScheduledTaskSettingsSet -RestartCount 999 -RestartInterval (New-TimeSpan -Minutes 1) -ExecutionTimeLimit ([TimeSpan]::Zero)
Register-ScheduledTask -TaskName "DataSyncClient" -Action $Action -Trigger $Trigger -Settings $Settings -RunLevel Highest -User "SYSTEM" -Force
Start-ScheduledTask -TaskName "DataSyncClient"
