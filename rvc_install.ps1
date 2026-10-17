$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$dataRoot = Join-Path $PSScriptRoot 'CCD_TTS'
$root = Join-Path $dataRoot 'rvc'
$py = Join-Path $root 'python'
$models = Join-Path $root 'models'
$installing = Join-Path $root 'installing.flag'
$installed = Join-Path $root 'installed.ok'
$failed = Join-Path $root 'install_failed.txt'
$log = Join-Path $root 'install.log'

New-Item -ItemType Directory -Force -Path $root,$models | Out-Null
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
    Set-Content -Encoding UTF8 -Path $log -Value 'CCD_TTS RVC installer'
    Log 'Starting installation. RVC is substantially larger than the other engines.'

    # rvc-python currently recommends Python 3.10 on Windows.
    $pythonVersion = '3.10.11'
    $zip = Join-Path $root 'python.zip'
    $pythonUrl = "https://www.python.org/ftp/python/$pythonVersion/python-$pythonVersion-embed-amd64.zip"
    # A running RVC API server keeps Python extension modules (.pyd files) locked on
    # Windows. Update/Repair can therefore fail while trying to replace the portable
    # runtime. Stop only Python processes whose executable lives inside this RVC
    # runtime before removing/replacing it.
    Log 'Stopping any running CCD_TTS RVC Python process before repair...'
    try {
        $pyPrefix = ([System.IO.Path]::GetFullPath($py)).TrimEnd('\') + '\'
        Get-CimInstance Win32_Process -Filter "Name='python.exe' OR Name='pythonw.exe'" -ErrorAction SilentlyContinue | ForEach-Object {
            $exePath = $_.ExecutablePath
            if ($exePath) {
                $fullExe = [System.IO.Path]::GetFullPath($exePath)
                if ($fullExe.StartsWith($pyPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
                    Log "Stopping RVC Python PID $($_.ProcessId): $fullExe"
                    Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue
                }
            }
        }
        Start-Sleep -Milliseconds 400
    } catch {
        Log "Warning: could not enumerate/stop an existing RVC Python process: $($_.Exception.Message)"
    }

    Log "Downloading portable Python $pythonVersion..."
    Invoke-WebRequest -UseBasicParsing -Uri $pythonUrl -OutFile $zip

    if (Test-Path $py) {
        $removed = $false
        for ($attempt = 1; $attempt -le 5 -and -not $removed; $attempt++) {
            try {
                Remove-Item $py -Recurse -Force -ErrorAction Stop
                $removed = $true
            } catch {
                if ($attempt -eq 5) { throw }
                Log "Portable Python folder is still locked; retrying removal ($attempt/5)..."
                Start-Sleep -Milliseconds 500
            }
        }
    }
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

    # fairseq 0.12.2 (required by rvc-python) uses legacy dependency metadata that
    # pip 24.1+ deliberately rejects. Pin pip 24.0 before resolving RVC.
    Log 'Pinning pip 24.0 for fairseq 0.12.2 compatibility...'
    Run-Native $pythonExe @('-m','pip','install','--disable-pip-version-check','--no-warn-script-location','--upgrade','pip==24.0') 'pip 24.0 compatibility pin failed' 

    # Detect NVIDIA robustly. nvidia-smi is not guaranteed to be on PATH even
    # when the NVIDIA driver is installed, so also inspect common locations and
    # Win32_VideoController. This is only a preference hint; CUDA availability is
    # verified again from inside the exact portable Python environment below.
    $hasNvidia = $false
    $nvidiaSmiCandidates = @(
        (Join-Path $env:SystemRoot 'System32\\nvidia-smi.exe'),
        (Join-Path $env:ProgramFiles 'NVIDIA Corporation\\NVSMI\\nvidia-smi.exe')
    )
    foreach ($candidate in $nvidiaSmiCandidates) {
        if ($candidate -and (Test-Path $candidate)) { $hasNvidia = $true; break }
    }
    if (-not $hasNvidia) {
        try {
            $gpuNames = Get-CimInstance Win32_VideoController -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Name
            if ($gpuNames | Where-Object { $_ -match 'NVIDIA' }) { $hasNvidia = $true }
        } catch {}
    }
    if (-not $hasNvidia) {
        $nvidiaSmi = Get-Command 'nvidia-smi.exe' -ErrorAction SilentlyContinue
        if ($nvidiaSmi) { $hasNvidia = $true }
    }

    # Install the compatible torch pair BEFORE fairseq. Installing fairseq first
    # lets pip satisfy its unconstrained torch dependency with the newest CPU-only
    # build, which is exactly what caused CUDA systems to end up in CPU mode.
    $cudaInstalled = $false
    if ($hasNvidia) {
        Log 'NVIDIA GPU detected; installing CUDA 11.8 PyTorch 2.1.1 / torchaudio 2.1.1...'
        try {
            Run-Native $pythonExe @('-m','pip','install','--disable-pip-version-check','--no-warn-script-location','--force-reinstall','--index-url','https://download.pytorch.org/whl/cu118','torch==2.1.1','torchaudio==2.1.1') 'CUDA PyTorch install failed'

            # Torch 2.1.x tries to initialize NumPy during import. In the fresh portable
            # environment NumPy is not installed yet, and Windows PowerShell 5.1 can
            # promote Torch's harmless stderr warning into a terminating NativeCommandError.
            # Install the exact NumPy version required by rvc-python before probing CUDA.
            Log 'Installing RVC-compatible NumPy before CUDA verification...'
            Run-Native $pythonExe @('-m','pip','install','--disable-pip-version-check','--no-warn-script-location','numpy==1.23.5') 'NumPy compatibility install failed'

            # Do not trust package installation alone. Confirm that the bundled CUDA
            # runtime can actually see a GPU. Run the probe through Run-Native so stderr
            # warnings cannot be mistaken for failure by Windows PowerShell 5.1.
            $cudaProbeFile = Join-Path $root 'cuda_probe.txt'
            Remove-Item $cudaProbeFile -Force -ErrorAction SilentlyContinue
            # Write a single pipe-delimited line. Earlier builds wrote literal \n text
            # into the probe file, so Get-Content saw one value like
            # "1\n11.8\nNVIDIA..." and incorrectly treated a working CUDA runtime
            # as unavailable.
            $probeCode = "import pathlib, torch; p=pathlib.Path(r'$($cudaProbeFile.Replace("'","''"))'); p.write_text(('1' if torch.cuda.is_available() else '0') + '|' + str(torch.version.cuda or 'none') + '|' + (torch.cuda.get_device_name(0) if torch.cuda.is_available() else 'no-device'), encoding='utf-8')"
            Run-Native $pythonExe @('-c',$probeCode) 'CUDA verification probe failed'
            $cudaProbeText = ''
            if (Test-Path $cudaProbeFile) { $cudaProbeText = (Get-Content -Raw $cudaProbeFile).Trim() }
            $cudaProbe = @($cudaProbeText -split '\|', 3)
            if ($cudaProbe.Count -ge 1 -and $cudaProbe[0].Trim() -eq '1') {
                $cudaInstalled = $true
                Log "CUDA probe succeeded: $($cudaProbe -join ' | ')"
            } else {
                Log "CUDA PyTorch installed but the runtime cannot see a GPU; falling back to CPU. Probe: $cudaProbeText"
            }
        } catch {
            Log "CUDA runtime install failed; falling back to CPU: $($_.Exception.Message)"
        }
    } else {
        Log 'No NVIDIA GPU detected; using CPU-compatible RVC runtime.'
    }
    if (-not $cudaInstalled) {
        Log 'Installing compatible CPU PyTorch 2.1.1 / torchaudio 2.1.1 runtime...'
        Run-Native $pythonExe @('-m','pip','install','--disable-pip-version-check','--no-warn-script-location','--force-reinstall','torch==2.1.1','torchaudio==2.1.1') 'compatible CPU PyTorch install failed'
    }

    # fairseq 0.12.2 does not provide an official Windows wheel on PyPI.
    # Building it from source commonly fails on Windows, so install a known
    # prebuilt CPython 3.10 / win_amd64 wheel. Use --no-deps here so fairseq
    # cannot replace the already-selected torch build with a newer CPU wheel.
    $fairseqWheel = Join-Path $root 'fairseq-0.12.2-cp310-cp310-win_amd64.whl'
    $fairseqUrl = 'https://github.com/BlueAmulet/fairseq-win-whl/releases/download/ci_build/fairseq-0.12.2-cp310-cp310-win_amd64.whl'
    Log 'Downloading prebuilt fairseq 0.12.2 Windows wheel...'
    Invoke-WebRequest -UseBasicParsing -Uri $fairseqUrl -OutFile $fairseqWheel
    if (-not (Test-Path $fairseqWheel)) { throw 'Failed to download the fairseq Windows wheel.' }

    Log 'Installing prebuilt fairseq 0.12.2 Windows wheel without changing the selected PyTorch runtime...'
    Run-Native $pythonExe @('-m','pip','install','--disable-pip-version-check','--no-warn-script-location','--no-deps',$fairseqWheel) 'prebuilt fairseq install failed'

    Log 'Installing rvc-python and remaining dependencies (this may take a long time and several GB)...'
    Run-Native $pythonExe @('-m','pip','install','--disable-pip-version-check','--no-warn-script-location','rvc-python==0.1.5') 'pip install rvc-python failed' 

    # Patch rvc-python's infer_file error handling. vc_single returns a tuple containing
    # the real traceback when conversion fails, while infer_file otherwise passes that
    # tuple to scipy.wavfile.write which hides the root cause behind
    # "tuple object has no attribute dtype". This converts that into the real error.
    $inferPy = Join-Path $py 'Lib\site-packages\rvc_python\infer.py'
    if (Test-Path $inferPy) {
        $inferText = Get-Content -Raw -Encoding UTF8 $inferPy
        $oldInfer = '        wavfile.write(output_path, self.vc.tgt_sr, wav_opt)'
        $newInfer = @'
        if isinstance(wav_opt, tuple):
            detail = wav_opt[0] if len(wav_opt) > 0 else "RVC conversion failed"
            raise RuntimeError(str(detail))
        wavfile.write(output_path, self.vc.tgt_sr, wav_opt)
'@.TrimEnd()
        if ($inferText.Contains($oldInfer)) {
            $inferText = $inferText.Replace($oldInfer, $newInfer)
            Set-Content -Encoding UTF8 -Path $inferPy -Value $inferText
            Log 'Applied RVC inference error-reporting compatibility patch.'
        }
    }

    Log 'Verifying RVC import and selecting the fastest available device...'
    Run-Native $pythonExe @('-c',"from rvc_python.infer import RVCInference; import torch; print('RVC import OK'); print('torch=' + torch.__version__); print('cuda_build=' + str(torch.version.cuda)); print('cuda_available=' + str(torch.cuda.is_available())); print('gpu=' + (torch.cuda.get_device_name(0) if torch.cuda.is_available() else 'none'))") 'RVC import verification failed'

    # Persist a runtime hint for CCD_TTS. CUDA is used only when this exact portable
    # Python environment can actually see a CUDA device; otherwise the mod uses CPU.
    $deviceHint = Join-Path $root 'device_hint.txt'
    $deviceHintEscaped = $deviceHint.Replace("'","''")
    Run-Native $pythonExe @('-c',"import pathlib, torch; pathlib.Path(r'$deviceHintEscaped').write_text('cuda:0' if torch.cuda.is_available() else 'cpu', encoding='ascii')") 'RVC device detection failed'
    $chosenDevice = (Get-Content -ErrorAction SilentlyContinue $deviceHint | Select-Object -First 1)
    if ($chosenDevice -eq 'cuda:0') { Log 'RVC device selected: CUDA GPU (cuda:0).' } else { Log 'RVC device selected: CPU.' }

    Set-Content -Encoding ASCII -Path $installed -Value 'rvc-python'
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
