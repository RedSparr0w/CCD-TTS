$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$dataRoot = Join-Path $PSScriptRoot 'CCD_TTS'
$root = Join-Path $dataRoot 'supertonic'
$py = Join-Path $root 'python'
$installing = Join-Path $root 'installing.flag'
$installed = Join-Path $root 'installed.ok'
$failed = Join-Path $root 'install_failed.txt'
$log = Join-Path $root 'install.log'

New-Item -ItemType Directory -Force -Path $root | Out-Null
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
    Set-Content -Encoding UTF8 -Path $log -Value "CCD_TTS Supertonic installer"
    Log 'Starting installation.'

    # Python 3.11 is deliberately used here; it has broad wheel support for
    # numpy/onnxruntime and is the version recommended by the archived ST3 setup.
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
    $lines = Get-Content $pth.FullName
    $lines = $lines | ForEach-Object { if ($_ -match '^#import site') { 'import site' } else { $_ } }
    if (-not ($lines -contains 'import site')) { $lines += 'import site' }
    Set-Content -Encoding ASCII $pth.FullName $lines

    $getPip = Join-Path $root 'get-pip.py'
    Log 'Downloading pip bootstrap...'
    Invoke-WebRequest -UseBasicParsing -Uri 'https://bootstrap.pypa.io/get-pip.py' -OutFile $getPip

    Log 'Installing pip...'
    Run-Native $pythonExe @($getPip, '--no-warn-script-location') 'get-pip.py failed'

    Log 'Installing Supertonic 1.3.1 server runtime...'
    Run-Native $pythonExe @('-m','pip','install','--disable-pip-version-check','--no-warn-script-location','--upgrade','supertonic[serve]==1.3.1') 'pip install supertonic failed'

    Log 'Verifying Supertonic import...'
    Run-Native $pythonExe @('-c',"import supertonic; print('supertonic import OK')") 'Supertonic import verification failed'

    Set-Content -Encoding ASCII -Path $installed -Value '1.3.1'
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
