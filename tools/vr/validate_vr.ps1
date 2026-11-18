# Runs a RELEASE PoseStudio build under the Khronos Vulkan validation layer with VR Mode started
# automatically (POSESTUDIO_VR_AUTOSTART), for the given number of seconds, and summarises what
# the layer reported — the correctness check of the stereo frame path that only a connected
# headset can exercise. Release builds request no layers themselves, so the LOADER injects it
# (VK_INSTANCE_LAYERS) and a settings file (VK_LAYER_SETTINGS_PATH) turns synchronization
# validation on and logs to a file; the legacy VK_LAYER_ENABLES token is ignored by SDK 1.4.350.
#
#   tools\vr\validate_vr.ps1 -Figure <preset.duf> [-Seconds 40] [-Exe build\Release\PoseStudio.exe]
#
# The figure to stand in the headset is -Figure, else the POSESTUDIO_TEST_FIGURE environment
# variable (what the scripted runs of tools\ikscripts read), else the script asks for it. There
# is no default: which figures a machine has is its owner's business.
#
# Put the headset on once the app is up (the session renders only while the headset is worn and
# focused); walk and grab a joint or two. The summary counts errors, synchronization hazards,
# warnings and performance notes; the 10 "OutputNotConsumed" performance notes — shadow.vert's
# eight and the splat occluder pipeline's two (mesh.vert's outputs depthcutout.frag does not
# read) — are the known baseline. The full log stays in the script's output folder
# (posestudio-vr-validation in the temp folder).
param(
    [string]$Figure = $env:POSESTUDIO_TEST_FIGURE,
    [int]$Seconds = 40,
    [string]$Exe = (Join-Path $PSScriptRoot "..\..\build\Release\PoseStudio.exe")
)
if ([string]::IsNullOrWhiteSpace($Figure)) {
    try {
        $Figure = Read-Host "Figure to load (the path of a figure preset, .duf)"
    } catch {
        $Figure = "" # (a host that cannot ask: the message below says how to name one)
    }
}
$Figure = "$Figure".Trim().Trim('"')
if ([string]::IsNullOrWhiteSpace($Figure) -or !(Test-Path -LiteralPath $Figure -PathType Leaf)) {
    Write-Host "validate_vr.ps1: no figure to load ('$Figure'). Name one with -Figure <preset.duf>, or set POSESTUDIO_TEST_FIGURE."
    exit 2
}
$out = Join-Path ([System.IO.Path]::GetTempPath()) "posestudio-vr-validation"
New-Item -ItemType Directory -Force $out | Out-Null
$logFile = Join-Path $out "validation.log"
if (Test-Path $logFile) { Remove-Item $logFile }
@"
khronos_validation.validate_sync = true
khronos_validation.debug_action = VK_DBG_LAYER_ACTION_LOG_MSG
khronos_validation.log_filename = $logFile
khronos_validation.report_flags = error,warn,perf,info
"@ | Out-File -Encoding ascii (Join-Path $out "vk_layer_settings.txt")

$env:VK_INSTANCE_LAYERS = "VK_LAYER_KHRONOS_validation"
$env:VK_LAYER_SETTINGS_PATH = $out
$env:POSESTUDIO_VR_AUTOSTART = "1"
$env:POSESTUDIO_VR_PERF = "1"
$env:POSESTUDIO_NO_PING = "1"
$env:POSESTUDIO_NO_UPDATE_CHECK = "1"
$env:QT_FORCE_STDERR_LOGGING = "1"
$appLog = Join-Path $out "app.log"
Write-Host "Launching $Exe for $Seconds s with VR Mode autostart; put the headset on now..."
$p = Start-Process -FilePath $Exe -ArgumentList ('"' + $Figure + '"') -PassThru -RedirectStandardError $appLog -RedirectStandardOutput (Join-Path $out "app-out.log")
Start-Sleep -Seconds $Seconds
if (!$p.HasExited) { Stop-Process -Id $p.Id -Force }
Start-Sleep -Seconds 2

Write-Host "--- [vr] lines ---"
Select-String -Path $appLog -Pattern "\[vr" | ForEach-Object { $_.Line } | Select-Object -First 40
if (Test-Path $logFile) {
    $t = Get-Content $logFile
    Write-Host "--- validation: $($t.Count) lines ---"
    Write-Host ("errors:   " + (($t | Select-String -Pattern "Validation Error" | Measure-Object).Count))
    Write-Host ("sync:     " + (($t | Select-String -Pattern "SYNC-HAZARD" | Measure-Object).Count))
    Write-Host ("warnings: " + (($t | Select-String -Pattern "Validation Warning" | Measure-Object).Count))
    Write-Host ("perf:     " + (($t | Select-String -Pattern "Validation Performance Warning" | Measure-Object).Count) + "  (10 = the known baseline: shadow.vert's eight, the splat occluder pipeline's two)")
    $t | Select-String -Pattern "Validation Error|SYNC-HAZARD" | Select-Object -First 10 | ForEach-Object { $_.Line.Substring(0, [Math]::Min(400, $_.Line.Length)) }
    Write-Host "full log: $logFile"
} else {
    Write-Host "no validation log was written (is the Vulkan SDK's validation layer installed?)"
}
