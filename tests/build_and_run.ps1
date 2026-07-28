# Build and run USB/IP protocol property tests
# Fetches GoogleTest and RapidCheck if not present, compiles tests with g++

$ErrorActionPreference = "Continue"
$TestDir = $PSScriptRoot
$DepsDir = "$TestDir\deps"

# Create deps directory
if (!(Test-Path $DepsDir)) { New-Item -ItemType Directory -Path $DepsDir | Out-Null }

# Fetch GoogleTest if not present
$GTestDir = "$DepsDir\googletest"
if (!(Test-Path "$GTestDir\googletest\include\gtest\gtest.h")) {
    Write-Host "Fetching GoogleTest..."
    & git clone --depth 1 --branch v1.14.0 https://github.com/google/googletest.git $GTestDir 2>&1 | Write-Host
}

# Fetch RapidCheck if not present
$RCDir = "$DepsDir\rapidcheck"
if (!(Test-Path "$RCDir\include\rapidcheck.h")) {
    Write-Host "Fetching RapidCheck..."
    & git clone --depth 1 https://github.com/emil-e/rapidcheck.git $RCDir 2>&1 | Write-Host
}

Write-Host "Compiling GoogleTest..."
$GTestSrc = "$GTestDir\googletest\src\gtest-all.cc"
$GTestMain = "$GTestDir\googletest\src\gtest_main.cc"
$GTestInc = "$GTestDir\googletest\include"
$GTestPrivInc = "$GTestDir\googletest"

# Compile gtest-all.cc to object file
& g++ -std=c++17 -O2 -c $GTestSrc -I $GTestInc -I $GTestPrivInc -o "$DepsDir\gtest-all.o"
if ($LASTEXITCODE -ne 0) { Write-Error "Failed to compile GoogleTest"; exit 1 }

Write-Host "Compiling RapidCheck..."
# Collect all RapidCheck source files
$RCSrcs = Get-ChildItem -Path "$RCDir\src" -Filter "*.cpp" -Recurse | ForEach-Object { $_.FullName }
$RCInc = "$RCDir\include"
$RCExtrasInc = "$RCDir\extras\gtest\include"

# Compile each RC source to object files
$RCObjs = @()
$idx = 0
foreach ($src in $RCSrcs) {
    $obj = "$DepsDir\rc_$idx.o"
    & g++ -std=c++17 -O2 -c $src -I $RCInc -I $GTestInc -o $obj
    if ($LASTEXITCODE -ne 0) { Write-Error "Failed to compile RapidCheck source: $src"; exit 1 }
    $RCObjs += $obj
    $idx++
}

Write-Host "Compiling test_protocol.cpp..."
$TestSrc = "$TestDir\test_protocol.cpp"
$TestObj = "$DepsDir\test_protocol.o"
$MocksDir = "$TestDir\mocks"

& g++ -std=c++17 -O2 -c $TestSrc `
    -I $GTestInc `
    -I $RCInc `
    -I $RCExtrasInc `
    -I $MocksDir `
    -I $TestDir `
    -o $TestObj
if ($LASTEXITCODE -ne 0) { Write-Error "Failed to compile test_protocol.cpp"; exit 1 }

Write-Host "Linking..."
$AllObjs = @("$DepsDir\gtest-all.o") + $RCObjs + @($TestObj)
$OutputExe = "$TestDir\test_protocol.exe"

& g++ -std=c++17 -O2 $AllObjs -o $OutputExe -lpthread
if ($LASTEXITCODE -ne 0) { Write-Error "Failed to link test binary"; exit 1 }

Write-Host "Running tests..."
& $OutputExe --gtest_color=yes
$exitCode = $LASTEXITCODE

exit $exitCode
