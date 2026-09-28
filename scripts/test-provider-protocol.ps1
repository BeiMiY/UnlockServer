$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$msbuild = & $vswhere -latest -products '*' -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
if (!$msbuild) { throw 'Visual Studio v143 C++ tools required.' }
$project = Join-Path $root 'tests\UnlockServer.Provider.Tests'
& $msbuild "$project\UnlockServer.Provider.Tests.vcxproj" /p:Configuration=Release /p:Platform=x64 /v:minimal /nologo
if ($LASTEXITCODE -ne 0) { throw 'Provider protocol test build failed.' }
$temp = Join-Path ([IO.Path]::GetTempPath()) ('UnlockServer-ProtocolTest-' + [guid]::NewGuid())
try {
    & "$project\bin\Release\UnlockServer.Provider.Tests.exe" $temp
    if ($LASTEXITCODE -ne 0) { throw 'Provider protocol tests failed.' }
} finally {
    if (Test-Path $temp) { Remove-Item -Recurse -Force $temp }
}
