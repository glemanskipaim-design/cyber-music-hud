# Captura o HUD para revisar o visual: dispara "Mostrar agora" e salva
# $Frames PNGs espaçados de $IntervalMs (as barras devem mudar entre eles se
# houver música tocando).
#
# Uso: powershell -ExecutionPolicy Bypass -File tools\snapshot.ps1 [-Frames 3] [-IntervalMs 600]
param(
    [int]$Frames = 3,
    [int]$IntervalMs = 600,
    [int]$FirstMs = 700
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Add-Type -Path (Join-Path $root 'tests\Native.cs') -ReferencedAssemblies System.Drawing
$exe = Join-Path $root 'build\CyberMusicHUD.exe'

$startedHere = $false
if ([Native]::FindWindow('CyberMusicHudHost', [NullString]::Value) -eq [IntPtr]::Zero) {
    Start-Process $exe
    $startedHere = $true
    Start-Sleep -Milliseconds 1500  # deixa o anúncio inicial (se houver) passar pelo detector
}

try {
    & $exe --show | Out-Null
    Start-Sleep -Milliseconds $FirstMs
    $hud = [Native]::FindWindow('CyberMusicHudOverlay', [NullString]::Value)
    for ($i = 1; $i -le $Frames; $i++) {
        $out = Join-Path $root "build\snapshot-$i.png"
        [Native]::Capture($hud, $out)
        Write-Host "ok: $out"
        if ($i -lt $Frames) { Start-Sleep -Milliseconds $IntervalMs }
    }
}
finally {
    if ($startedHere) { & $exe --exit }
}
