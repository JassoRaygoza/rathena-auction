# Generate Testing_Auction_X2.exe + auction_x2.grf from the Auction window sources.
# Never modifies the original EXE; every run writes a new folder under output\.
# Client resources (skin textures, button bitmaps) live outside the emulator repository.
# Defaults expect sibling folders: rathena-auction (emulator), rathena-auction-resources
# and rathena-auction-client (this tool).
param([string]$Exe, [string]$MSBuild, [string]$Resources, [string]$Emulator)
$ErrorActionPreference = 'Stop'
$build = $PSScriptRoot
if (!$Exe) { throw 'Pass -Exe with the path to the original Testing.exe' }
if (!$Resources) { $Resources = Join-Path $build '..\..\..\rathena-auction-resources' }
if (!$Emulator) { $Emulator = Join-Path $build '..\..\..\rathena-auction' }
$Textures = Join-Path $Resources 'data\texture'
$Buttons = Join-Path $Resources 'buttons'
if (!$MSBuild) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $MSBuild = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
}
foreach ($required in @($Exe, $MSBuild, $Textures, $Buttons, (Join-Path $Emulator 'src\common\auction_protocol.hpp'))) {
    if (!$required -or !(Test-Path -LiteralPath $required)) { throw "Required path unavailable: $required" }
}
$original = (Get-FileHash -LiteralPath $Exe -Algorithm SHA256).Hash.ToLower()
if ($original -ne '95fbd61d250d3d7481b271d7d1928b2a03e8ebc9fae68fdeb68f2f74496e5a65') { throw 'Unsupported original EXE' }
$patch = Join-Path $build ('output\' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
$resourcesOut = Join-Path $patch 'resources'
New-Item -ItemType Directory -Path $patch -Force | Out-Null
$env:PYTHONIOENCODING = 'utf-8'
function Invoke-Checked([string]$Label, [scriptblock]$Command) {
    & $Command
    if ($LASTEXITCODE -ne 0) { throw "$Label failed with exit code $LASTEXITCODE" }
}
Invoke-Checked 'build section' { & $MSBuild "$build\auction_section.vcxproj" /p:Configuration=Release /p:Platform=Win32 "/p:PatchOutput=$patch" "/p:EmulatorDir=$Emulator" /nologo /verbosity:minimal }
& "$build\scale_skin.ps1" -Textures $Textures -Output $resourcesOut
$buttonTarget = (Get-ChildItem -LiteralPath $resourcesOut -Recurse -File -Filter auction_bg_0.bmp).Directory.FullName
Get-ChildItem -LiteralPath $Buttons -File -Filter '*.bmp' | Copy-Item -Destination $buttonTarget
Invoke-Checked 'package X2 skin' { python "$build\auction_skin_grf.py" $resourcesOut --out "$patch\auction_x2.grf" --buttons-manifest "$Buttons\manifest.json" }
$output = Join-Path $patch 'Testing_Auction_X2.exe'
Invoke-Checked 'patch' { python "$build\auction_exe_patch.py" $Exe --section-image "$patch\auction_section.dll" --out $output --manifest "$patch\exe_patch_manifest.json" }
if ((Get-FileHash -LiteralPath $Exe -Algorithm SHA256).Hash.ToLower() -ne $original) { throw 'Original Testing.exe changed' }
"Original Testing.exe unchanged: $original"
"EXE: $output"
"GRF: $patch\auction_x2.grf"
