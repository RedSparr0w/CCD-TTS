$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$dataRoot = Join-Path $PSScriptRoot 'CCD_TTS'
$root = Join-Path $dataRoot 'rvc'
$py = Join-Path $root 'python'
$pythonExe = Join-Path $py 'python.exe'
$deviceHint = Join-Path $root 'device_hint.txt'
$log = Join-Path $root 'quick_acceleration_test.log'

function Write-Log([string]$text) {
    $stamp = Get-Date -Format 'yyyy-MM-dd HH:mm:ss'
    Add-Content -Encoding UTF8 -Path $log -Value "[$stamp] $text"
    Write-Host $text
}

function Run-Native([string]$exe, [string[]]$arguments, [string]$failureText) {
    $oldPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & $exe @arguments 2>&1 | Tee-Object -FilePath $log -Append
        $code = $LASTEXITCODE
    }
    finally { $ErrorActionPreference = $oldPreference }
    if ($code -ne 0) { throw "$failureText (exit code $code)" }
}

if (-not (Test-Path $pythonExe)) {
    throw "RVC portable Python is not installed: $pythonExe"
}

Set-Content -Encoding UTF8 -Path $log -Value 'CCD_TTS RVC quick acceleration test'
Write-Log 'Using the existing RVC environment. No Python/fairseq/RVC reinstall will be performed.'

# Stop only the CCD_TTS RVC runtime so torch DLLs are not locked.
try {
    $pyPrefix = ([System.IO.Path]::GetFullPath($py)).TrimEnd('\') + '\'
    Get-CimInstance Win32_Process -Filter "Name='python.exe' OR Name='pythonw.exe'" -ErrorAction SilentlyContinue | ForEach-Object {
        if ($_.ExecutablePath) {
            $fullExe = [System.IO.Path]::GetFullPath($_.ExecutablePath)
            if ($fullExe.StartsWith($pyPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
                Write-Log "Stopping RVC Python PID $($_.ProcessId)"
                Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue
            }
        }
    }
    Start-Sleep -Milliseconds 400
} catch {}

Write-Log 'Current runtime:'
Run-Native $pythonExe @('-c',"import torch; print('torch=' + torch.__version__); print('cuda_build=' + str(torch.version.cuda)); print('cuda_available=' + str(torch.cuda.is_available())); print('gpu=' + (torch.cuda.get_device_name(0) if torch.cuda.is_available() else 'none'))") 'Torch probe failed'

$hasNvidia = $false
try {
    $gpuNames = Get-CimInstance Win32_VideoController -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Name
    if ($gpuNames | Where-Object { $_ -match 'NVIDIA' }) { $hasNvidia = $true }
} catch {}
if (-not $hasNvidia) {
    $nvidiaSmi = Get-Command 'nvidia-smi.exe' -ErrorAction SilentlyContinue
    if ($nvidiaSmi) { $hasNvidia = $true }
}

if ($hasNvidia) {
    Write-Log 'NVIDIA GPU detected. Installing/replacing only torch + torchaudio with the CUDA 11.8 build.'
    Write-Log 'This should use the pip cache if the CUDA wheel was already downloaded.'
    Run-Native $pythonExe @('-m','pip','install','--disable-pip-version-check','--no-warn-script-location','--force-reinstall','--index-url','https://download.pytorch.org/whl/cu118','torch==2.1.1','torchaudio==2.1.1') 'CUDA PyTorch repair failed'
    Run-Native $pythonExe @('-m','pip','install','--disable-pip-version-check','--no-warn-script-location','numpy==1.23.5') 'NumPy compatibility check failed'
} else {
    Write-Log 'No NVIDIA GPU detected. Keeping CPU mode.'
}

$probeFile = Join-Path $root 'quick_cuda_probe.txt'
$probeCode = "import pathlib, torch; p=pathlib.Path(r'$($probeFile.Replace("'","''"))'); p.write_text(('1' if torch.cuda.is_available() else '0') + '|' + str(torch.version.cuda or 'none') + '|' + (torch.cuda.get_device_name(0) if torch.cuda.is_available() else 'no-device'), encoding='utf-8')"
Run-Native $pythonExe @('-c',$probeCode) 'CUDA verification probe failed'
$probeText = (Get-Content -Raw $probeFile).Trim()
$parts = @($probeText -split '\|',3)
Write-Log "Probe: $($parts -join ' | ')"

if ($parts.Count -ge 1 -and $parts[0] -eq '1') {
    Set-Content -Encoding ASCII -Path $deviceHint -Value 'cuda:0'
    Write-Log 'SUCCESS: RVC will use cuda:0.'
} else {
    Set-Content -Encoding ASCII -Path $deviceHint -Value 'cpu'
    Write-Log 'CUDA is not available to this RVC environment; CPU mode retained.'
}

Write-Host ''
Write-Host "Log: $log"
Read-Host 'Press Enter to close'
