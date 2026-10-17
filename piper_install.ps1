$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$dataRoot = Join-Path $PSScriptRoot 'CCD_TTS'
$root = Join-Path $dataRoot 'piper'
$py = Join-Path $root 'python'
$voices = Join-Path $root 'voices'
$installing = Join-Path $root 'installing.flag'
$installed = Join-Path $root 'installed.ok'
$failed = Join-Path $root 'install_failed.txt'
$log = Join-Path $root 'install.log'

New-Item -ItemType Directory -Force -Path $root,$voices | Out-Null
Remove-Item $failed -Force -ErrorAction SilentlyContinue
Remove-Item $installed -Force -ErrorAction SilentlyContinue
Set-Content -Encoding ASCII -Path $installing -Value 'installing'

function Log([string]$text) {
    $stamp = Get-Date -Format 'yyyy-MM-dd HH:mm:ss'
    Add-Content -Encoding UTF8 -Path $log -Value "[$stamp] $text"
}

function Run-Native([string]$exe, [string[]]$arguments, [string]$failureText) {
    # Run console tools without creating a visible console window. Redirect stdout
    # and stderr to temporary files so PowerShell 5.1 cannot promote harmless
    # native stderr warnings into terminating ErrorRecords.
    $stdout = Join-Path $root ('native_stdout_' + [guid]::NewGuid().ToString('N') + '.log')
    $stderr = Join-Path $root ('native_stderr_' + [guid]::NewGuid().ToString('N') + '.log')
    try {
        $quoted = @()
        foreach ($arg in $arguments) {
            if ($null -eq $arg) { $quoted += '""'; continue }
            if ($arg -match '[\s"]') {
                $quoted += ('"' + ($arg -replace '"','\\"') + '"')
            } else {
                $quoted += $arg
            }
        }
        $argLine = [string]::Join(' ', $quoted)
        $proc = Start-Process -FilePath $exe -ArgumentList $argLine -WindowStyle Hidden -Wait -PassThru `
            -RedirectStandardOutput $stdout -RedirectStandardError $stderr
        if (Test-Path $stdout) { Get-Content $stdout -ErrorAction SilentlyContinue | Add-Content -Encoding UTF8 -Path $log }
        if (Test-Path $stderr) { Get-Content $stderr -ErrorAction SilentlyContinue | Add-Content -Encoding UTF8 -Path $log }
        if ($proc.ExitCode -ne 0) { throw "$failureText (exit code $($proc.ExitCode))" }
    }
    finally {
        Remove-Item $stdout,$stderr -Force -ErrorAction SilentlyContinue
    }
}

try {
    Set-Content -Encoding UTF8 -Path $log -Value 'CCD_TTS Piper installer'
    Log 'Starting installation.'

    $pythonVersion = '3.11.9'
    $zip = Join-Path $root 'python.zip'
    $pythonUrl = "https://www.python.org/ftp/python/$pythonVersion/python-$pythonVersion-embed-amd64.zip"
    Log "Downloading portable Python $pythonVersion..."
    Invoke-WebRequest -UseBasicParsing -Uri $pythonUrl -OutFile $zip

    if (Test-Path $py) { Remove-Item $py -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $py | Out-Null
    Expand-Archive -Force -Path $zip -DestinationPath $py
    $pythonExe = Join-Path $py 'python.exe'
    if (-not (Test-Path $pythonExe)) { throw 'Portable Python extraction did not produce python.exe.' }

    $pth = Get-ChildItem $py -Filter 'python*._pth' | Select-Object -First 1
    if (-not $pth) { throw 'Portable Python _pth file was not found.' }
    $lines = Get-Content $pth.FullName | ForEach-Object { if ($_ -match '^#import site') { 'import site' } else { $_ } }
    if (-not ($lines -contains 'import site')) { $lines += 'import site' }
    Set-Content -Encoding ASCII $pth.FullName $lines

    $getPip = Join-Path $root 'get-pip.py'
    Log 'Downloading pip bootstrap...'
    Invoke-WebRequest -UseBasicParsing -Uri 'https://bootstrap.pypa.io/get-pip.py' -OutFile $getPip
    Run-Native $pythonExe @($getPip, '--no-warn-script-location') 'get-pip.py failed'

    Log 'Installing Piper HTTP runtime...'
    Run-Native $pythonExe @('-m','pip','install','--disable-pip-version-check','--no-warn-script-location','--upgrade','piper-tts[http]') 'pip install piper-tts[http] failed'

    Log 'Downloading default en_US-lessac-medium voice...'
    Run-Native $pythonExe @('-m','piper.download_voices','--data-dir',$voices,'en_US-lessac-medium') 'Default Piper voice download failed'

    Run-Native $pythonExe @('-c',"import piper; import flask; print('Piper import OK')") 'Piper import verification failed'

    Set-Content -Encoding ASCII -Path $installed -Value 'piper-tts'
    Log 'Installation completed successfully.'
}
catch {
    $message = $_.Exception.Message
    Log "ERROR: $message"
    Set-Content -Encoding UTF8 -Path $failed -Value $message
}
finally {
    Remove-Item $installing -Force -ErrorAction SilentlyContinue
}
