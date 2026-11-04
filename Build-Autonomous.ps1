param(
    [ValidateSet('new','djules')][string]$TestRoute = 'new',
    [string]$DependencyRoot = '',
    [string]$BuildRoot = '',
    [switch]$RunTests,
    # 0.2 V576: first build the 32-bit layer (Win32, into <BuildRoot>-x86),
    # which the x64 build bundles in ofxr_x86 for 32-bit games.
    [switch]$X86Layer
)
# Native compiler diagnostics on stderr are not PowerShell failures.
# Every configure/build/test exit code is checked explicitly below.
$ErrorActionPreference = 'Continue'
$sourceRoot = $PSScriptRoot
if (-not $DependencyRoot) { $DependencyRoot = [IO.Path]::GetFullPath((Join-Path $sourceRoot '../.local/ofxr-autonomous-deps')) }
if (-not $BuildRoot) { $BuildRoot = [IO.Path]::GetFullPath((Join-Path $sourceRoot "../builds/ofxr-autonomous-$TestRoute")) }
$cmake = 'C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
$ctest = Join-Path (Split-Path $cmake) 'ctest.exe'
if ($X86Layer) {
    $x86Root = $BuildRoot + '-x86'
    & $cmake -S $sourceRoot -B $x86Root -G 'Visual Studio 17 2022' -A Win32 "-DXRFG_DEPENDENCY_ROOT=$DependencyRoot" -DXRFG_BUILD_TESTS=OFF -DXRFG_BUILD_STANDALONE=OFF
    if ($LASTEXITCODE) { throw "32-bit layer configure failed: $LASTEXITCODE" }
    & $cmake --build $x86Root --config Release --target XR_APILAYER_XRFrameBridge_diagnostic --parallel
    if ($LASTEXITCODE) { throw "32-bit layer build failed: $LASTEXITCODE" }
}
& $cmake -S $sourceRoot -B $BuildRoot -G 'Visual Studio 17 2022' -A x64 "-DXRFG_DEPENDENCY_ROOT=$DependencyRoot" "-DXRFG_TEST_NVIDIA_ROUTE=$TestRoute" "-DXRFG_AFW_RUNTIME=$DependencyRoot/runtime/afw-beta6/PDAFWPlugin.dll" -DXRFG_BUILD_TESTS=ON
if ($LASTEXITCODE) { throw "CMake configure failed: $LASTEXITCODE" }
& $cmake --build $BuildRoot --config Release --parallel
if ($LASTEXITCODE) { throw "Release build failed: $LASTEXITCODE" }
if ($RunTests) {
    & $ctest --test-dir $BuildRoot -C Release --output-on-failure --timeout 60
    if ($LASTEXITCODE) { throw "CTest failed: $LASTEXITCODE" }
}
