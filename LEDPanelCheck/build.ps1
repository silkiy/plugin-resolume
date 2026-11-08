$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$projectDir = $PSScriptRoot
$buildDir = Join-Path $projectDir '.build'
$sdkRevision = '5b6e07840277df9ec35a228b9eb067636316350f'
$sdkDir = Join-Path $buildDir "ffgl-$sdkRevision"
$sdkArchive = Join-Path $buildDir "ffgl-$($sdkRevision.Substring(0, 7)).zip"
$sdkHash = '668F61EA84F61AC2A1532B83BEDE2F15B261D3DA71EAD43A9FD4A7B4A8619474'

$cmake = (Get-Command cmake.exe -ErrorAction SilentlyContinue).Source
if (!$cmake) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswhere) {
        $cmake = & $vswhere -products '*' -version '[17.0,18.0)' `
            -find 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' |
            Select-Object -First 1
    }
}
if (!$cmake) {
    throw 'Install CMake 3.21+ and the Visual Studio 2022 Desktop development with C++ workload.'
}

$sdkSource = Join-Path $sdkDir 'source\lib\FFGLSDK.cpp'
$glewLibrary = Join-Path $sdkDir 'deps\glew-2.1.0\lib\Release\x64\glew32s.lib'
if (!(Test-Path -LiteralPath $sdkSource) -or !(Test-Path -LiteralPath $glewLibrary)) {
    New-Item -ItemType Directory -Path $buildDir -Force | Out-Null
    if (!(Test-Path -LiteralPath $sdkArchive)) {
        & curl.exe --fail --location --silent --show-error --output $sdkArchive `
            "https://codeload.github.com/resolume/ffgl/zip/$sdkRevision"
        if ($LASTEXITCODE -ne 0) {
            Remove-Item -LiteralPath $sdkArchive -ErrorAction SilentlyContinue
            throw 'Could not download the Resolume FFGL SDK.'
        }
    }
    if ((Get-FileHash -LiteralPath $sdkArchive -Algorithm SHA256).Hash -ne $sdkHash) {
        throw "FFGL archive checksum mismatch. Delete $sdkArchive before retrying."
    }
    Expand-Archive -LiteralPath $sdkArchive -DestinationPath $buildDir -Force
}

$cmakeDir = Join-Path $buildDir 'cmake'
& $cmake -S $projectDir -B $cmakeDir -G 'Visual Studio 17 2022' -A x64 "-DFFGL_SDK_DIR=$sdkDir"
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }

& $cmake --build $cmakeDir --config Release --target LEDPanelCheck
if ($LASTEXITCODE -ne 0) { throw 'LEDPanelCheck.dll failed to build.' }

Write-Host (Join-Path $projectDir 'dist\LEDPanelCheck.dll')
