# Teste ponta a ponta sem injetar teclas: sobe o app, pede "Mostrar agora"
# e confere que o HUD aparece no canto inferior direito, não rouba o foco,
# é transparente a clique e some sozinho dentro do prazo.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Add-Type -Path (Join-Path $root 'tests\Native.cs') -ReferencedAssemblies System.Drawing
Add-Type -AssemblyName System.Windows.Forms
$exe = Join-Path $root 'build\CyberMusicHUD.exe'
$failures = 0
function Check($ok, $what) {
    if ($ok) { Write-Host "  ok   $what" } else { Write-Host "  FALHOU $what"; $script:failures++ }
}

if ([Native]::FindWindow('CyberMusicHudHost', [NullString]::Value) -ne [IntPtr]::Zero) {
    & $exe --exit; Start-Sleep -Milliseconds 500
}
$proc = Start-Process $exe -PassThru
Start-Sleep -Milliseconds 1500
try {
    Check ([Native]::FindWindow('CyberMusicHudHost', [NullString]::Value) -ne [IntPtr]::Zero) 'app no ar (janela host)'
    $hud = [Native]::FindWindow('CyberMusicHudOverlay', [NullString]::Value)
    Check ($hud -ne [IntPtr]::Zero) 'janela do HUD criada'

    # Segunda instância sai na hora, sem duplicar.
    $second = Start-Process $exe -PassThru
    Check ($second.WaitForExit(3000) -and -not $proc.HasExited) 'instância única'

    & $exe --show | Out-Null
    Start-Sleep -Milliseconds 500
    Check ([Native]::IsWindowVisible($hud)) 'HUD visível após --show'
    # Foco: o requisito é o HUD nunca ficar em primeiro plano. Comparar com a
    # janela de antes dá falso positivo se o usuário trocar de janela no meio.
    Check ([Native]::ForegroundPid() -ne $proc.Id) 'foco não foi para o app'

    $ex = [Native]::ExStyle($hud)
    foreach ($flag in 'WS_EX_TRANSPARENT', 'WS_EX_NOACTIVATE', 'WS_EX_TOPMOST', 'WS_EX_TOOLWINDOW', 'WS_EX_LAYERED') {
        Check (($ex -band [Native]::$flag) -ne 0) "estilo $flag"
    }

    $r = [Native]::Rect($hud)
    $screen = [System.Windows.Forms.Screen]::PrimaryScreen.WorkingArea
    Check ($r.Right -le $screen.Right -and $r.Right -gt $screen.Right - 120) 'colado à direita da área de trabalho'
    Check ($r.Bottom -le $screen.Bottom -and $r.Bottom -gt $screen.Bottom - 120) 'colado embaixo da área de trabalho'

    Start-Sleep -Milliseconds 6000
    Check (-not [Native]::IsWindowVisible($hud)) 'some sozinho em ~6 s'
    Check ([Native]::ForegroundPid() -ne $proc.Id) 'foco não foi para o app no fim'
}
finally {
    & $exe --exit
    if (-not $proc.WaitForExit(3000)) { $proc.Kill(); Write-Host '  FALHOU --exit não encerrou'; $failures++ }
}
if ($failures) { Write-Host "e2e: $failures falha(s)"; exit 1 }
Write-Host 'e2e: tudo ok'
