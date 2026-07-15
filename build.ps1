$ErrorActionPreference = 'Stop'

New-Item -ItemType Directory -Force -Path "$PSScriptRoot\build" | Out-Null

$vcvars = Get-ChildItem `
    'C:\Program Files\Microsoft Visual Studio', `
    'C:\Program Files (x86)\Microsoft Visual Studio' `
    -Filter vcvars64.bat -Recurse -ErrorAction SilentlyContinue |
    Sort-Object FullName -Descending |
    Select-Object -First 1

if (-not $vcvars) {
    throw 'Visual Studio Build Tools with vcvars64.bat were not found.'
}

$source = "$PSScriptRoot\gabserve.cpp"
$output = "$PSScriptRoot\build\gabserve.exe"
$object = "$PSScriptRoot\build\gabserve.obj"
$command = 'call "{0}" >nul && cl /nologo /std:c++17 /EHsc /W4 /DUNICODE /D_UNICODE /Fo:"{1}" /Fe:"{2}" "{3}" /link /subsystem:console ws2_32.lib ole32.lib oleaut32.lib windowscodecs.lib user32.lib gdi32.lib winmm.lib uuid.lib' -f $vcvars.FullName, $object, $output, $source

& cmd.exe /d /c $command

if ($LASTEXITCODE -ne 0) {
    throw "Compilation failed with exit code $LASTEXITCODE."
}

Write-Host "Built: $PSScriptRoot\build\gabserve.exe"
