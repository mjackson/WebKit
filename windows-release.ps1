param(
    # not on by default because CI does not need it (for some reason)
    # and i really really really really do not want to break anything
    #
    # you almost certainly need this to build locally
    [switch][bool]$ExtraEffortPathManagement = $false,
    
    # Install dependencies automatically (useful for CI)
    [switch][bool]$InstallDependencies = $false
)
$ErrorActionPreference = "Stop"

# Install dependencies if requested (for CI/fresh environments)
if ($InstallDependencies) {
    Write-Host ":: Installing build dependencies"
    
    # Check if scoop is installed
    if (-not (Get-Command scoop -ErrorAction SilentlyContinue)) {
        Write-Host "  Installing Scoop package manager..."
        Invoke-RestMethod -Uri https://get.scoop.sh | Invoke-Expression
    }
    
    # Install required tools via scoop
    $RequiredTools = @("cmake", "ninja", "python", "ruby", "perl", "make")
    foreach ($tool in $RequiredTools) {
        if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
            Write-Host "  Installing $tool..."
            scoop install $tool
        }
    }
    
    # Install vcpkg if not present
    if (-not (Test-Path "C:\Users\$env:USERNAME\scoop\apps\vcpkg")) {
        Write-Host "  Installing vcpkg..."
        scoop install vcpkg
    }
}

# Set up MSVC environment variables. This is taken from Bun's 'scripts\env.ps1'
# Check for ARM64 environment
$processor = Get-WmiObject Win32_Processor
$isARM64 = ($processor.Architecture -eq 12 -or $env:BUILD_ARM64 -eq "1")

if ($isARM64) {
    Write-Host "Building for ARM64 architecture"
}

# Try to load VS environment if available
if ($env:VSINSTALLDIR -eq $null) {
    Write-Host "Loading Visual Studio environment, this may take a second..."
    # Check both Program Files locations for VS 2022
    $vsDir = $null
    if (Test-Path "C:\Program Files\Microsoft Visual Studio\2022") {
        $vsDir = Get-ChildItem -Path "C:\Program Files\Microsoft Visual Studio\2022" -Directory | Select-Object -First 1
    } elseif (Test-Path "C:\Program Files (x86)\Microsoft Visual Studio\2022") {
        $vsDir = Get-ChildItem -Path "C:\Program Files (x86)\Microsoft Visual Studio\2022" -Directory | Select-Object -First 1
    }
    
    if ($vsDir -ne $null -and (Test-Path (Join-Path -Path $vsDir.FullName -ChildPath "Common7\Tools\Launch-VsDevShell.ps1"))) {
        Push-Location $vsDir.FullName
        try {
            # Detect the target architecture
            $targetArch = "amd64"
            $hostArch = "amd64"
            
            if ($isARM64) {
                $targetArch = "arm64"
                # Use x64 host tools if running under emulation
                if ($env:PROCESSOR_ARCHITECTURE -eq "AMD64") {
                    $hostArch = "amd64"
                } else {
                    $hostArch = "arm64"
                }
            }
            
            Write-Host "Target Architecture: $targetArch, Host Architecture: $hostArch"
            . (Join-Path -Path $vsDir.FullName -ChildPath "Common7\Tools\Launch-VsDevShell.ps1") -Arch $targetArch -HostArch $hostArch
        }
        finally { Pop-Location }
    } else {
        Write-Host "Visual Studio developer shell not found, using clang-cl directly"
    }
}

if ($Env:VSCMD_ARG_TGT_ARCH -eq "x86") {
    # Please do not try to compile Bun for 32 bit. It will not work. I promise.
    throw "Visual Studio environment is targetting 32 bit. This configuration is definetly a mistake."
}

# Set COMSPEC to ensure we use Windows command processor
$env:COMSPEC = "$env:SystemRoot\system32\cmd.exe"

# Clean up PATH - remove all Git/MSYS/Cygwin/MinGW paths that cause conflicts
Write-Host "Original PATH: $env:PATH"

$CleanPaths = @()
$SplitPath = $env:PATH -split ";"

foreach ($path in $SplitPath) {
    if ($path -notlike "*Git*" -and 
        $path -notlike "*mingw*" -and 
        $path -notlike "*msys*" -and 
        $path -notlike "*cygwin*" -and
        $path -notlike "*strawberry*") {
        $CleanPaths += $path
    }
}

# Add essential Windows and tool paths in correct order
$EssentialPaths = @(
    "C:\Program Files\LLVM\bin",
    "$env:SystemRoot\system32",
    "$env:SystemRoot",
    "$env:SystemRoot\System32\Wbem",
    "$env:SystemRoot\System32\WindowsPowerShell\v1.0",
    "C:\Users\$env:USERNAME\scoop\shims",
    "C:\Users\$env:USERNAME\scoop\apps\cmake\current\bin",
    "C:\Users\$env:USERNAME\scoop\apps\ninja\current",
    "C:\Users\$env:USERNAME\scoop\apps\python\current",
    "C:\Users\$env:USERNAME\scoop\apps\python\current\Scripts",
    "C:\Users\$env:USERNAME\scoop\apps\ruby\current\bin",
    "C:\Users\$env:USERNAME\scoop\apps\perl\current\perl\bin",
    "C:\Users\$env:USERNAME\scoop\apps\perl\current\perl\site\bin",
    "C:\Users\$env:USERNAME\scoop\apps\make\current\bin"
)

# Combine paths, removing duplicates
$FinalPaths = $EssentialPaths
foreach ($path in $CleanPaths) {
    if ($path -and $FinalPaths -notcontains $path) {
        $FinalPaths += $path
    }
}

$env:PATH = $FinalPaths -join ";"
Write-Host "Cleaned PATH: $env:PATH"

# Find make executable
$MakeExe = $null
try {
    $MakeExe = (Get-Command make -ErrorAction SilentlyContinue).Path
} catch { }
if (-not $MakeExe) {
    Write-Host "make not found, installing via scoop..."
    scoop install make
    $MakeExe = (Get-Command make).Path
}

# Keep a copy of PATH with Perl for later use (some WebKit scripts need it)
$PathWithPerl = $env:PATH

if($ExtraEffortPathManagement) {
    $SedPath = $(& cygwin.exe -c 'where sed')
    $SedDir = Split-Path $SedPath
    $LinkPath = $(gcm link).Path
    $LinkDir = Split-Path $LinkPath

    # override coreutils link with the msvc one
    $env:PATH = "$LinkDir;$SedDir;$PathWithPerl"
}

Write-Host $env:PATH

# Show clang-cl version but don't fail if link isn't found (we'll use lld-link)
try {
    $linkPath = (Get-Command link -ErrorAction SilentlyContinue).Path
    if ($linkPath) {
        Write-Host "Found link at: $linkPath"
    }
} catch { }

clang-cl.exe --version

$env:CC = "clang-cl"
$env:CXX = "clang-cl"

$output = if ($env:WEBKIT_OUTPUT_DIR) { $env:WEBKIT_OUTPUT_DIR } else { "bun-webkit" }
$WebKitBuild = if ($env:WEBKIT_BUILD_DIR) { $env:WEBKIT_BUILD_DIR } else { "WebKitBuild" }
$CMAKE_BUILD_TYPE = if ($env:CMAKE_BUILD_TYPE) { $env:CMAKE_BUILD_TYPE } else { "Release" }
$BUN_WEBKIT_VERSION = if ($env:BUN_WEBKIT_VERSION) { $env:BUN_WEBKIT_VERSION } else { $(git rev-parse HEAD) }

# Use vcpkg for ICU - packages are installed in project directory
$UseVcpkg = $true
$VcpkgRoot = if ($env:VCPKG_ROOT) { $env:VCPKG_ROOT } else { "C:\Users\$env:USERNAME\scoop\apps\vcpkg\current" }

# vcpkg installs packages in the project directory when using manifest mode
$VcpkgInstalled = Join-Path (Get-Location) "vcpkg_installed\arm64-windows-static"

# Set up ICU paths based on whether we're using vcpkg or building from source
if ($UseVcpkg) {
    Write-Host ":: Using vcpkg for ICU"
    
    # Ensure vcpkg dependencies are installed with static CRT
    if (-not (Test-Path "$VcpkgInstalled\include\unicode")) {
        Write-Host ":: Installing ICU via vcpkg with static CRT"
        
        # Create custom triplet if it doesn't exist  
        $TripletFile = "arm64-windows-static.cmake"
        if (-not (Test-Path $TripletFile)) {
            $TripletContent = @"
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE static)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Windows)
"@
            Set-Content -Path $TripletFile -Value $TripletContent
        }
        
        & vcpkg install --triplet arm64-windows-static
        if ($LASTEXITCODE -ne 0) { throw "vcpkg install failed with exit code $LASTEXITCODE" }
    }
    
    $ICU_STATIC_ROOT = $VcpkgInstalled
    $ICU_STATIC_LIBRARY = Join-Path $ICU_STATIC_ROOT "lib"
    $ICU_STATIC_INCLUDE_DIR = Join-Path $ICU_STATIC_ROOT "include"
} else {
    Write-Host ":: Building ICU from source"
    # Original ICU build code would go here, but we'll skip it for now
    throw "Manual ICU build not implemented - please install vcpkg"
}

$null = mkdir $WebKitBuild -ErrorAction SilentlyContinue

Write-Host ":: Configuring WebKit"

$env:PATH = $PathWithPerl

$env:CFLAGS = "/Zi"
$env:CXXFLAGS = "/Zi"

# Set CC and CXX for the inspector preprocessor script
$env:CC = "clang-cl"
$env:CXX = "clang-cl"

$CmakeMsvcRuntimeLibrary = "MultiThreaded"
if ($CMAKE_BUILD_TYPE -eq "Debug") {
    $CmakeMsvcRuntimeLibrary = "MultiThreadedDebug"
}

$NoWebassembly = if ($env:NO_WEBASSEMBLY) { $env:NO_WEBASSEMBLY } else { $false }
$WebAssemblyState = if ($NoWebassembly) { "OFF" } else { "ON" }

# Set architecture for CMake
$CmakeArch = if ($isARM64) { "ARM64" } else { "x64" }

# Set vcpkg toolchain file if using vcpkg
$VcpkgToolchain = if ($UseVcpkg) { 
    "-DCMAKE_TOOLCHAIN_FILE=$VcpkgRoot/scripts/buildsystems/vcpkg.cmake",
    "-DVCPKG_TARGET_TRIPLET=arm64-windows-static",
    "-DVCPKG_OVERLAY_TRIPLETS=$VcpkgRoot/triplets/community"
} else { @() }

# Set Ruby path explicitly and ensure required gems are installed
$RubyPath = "C:\Users\$env:USERNAME\scoop\apps\ruby\current\bin\ruby.exe"
$GemPath = "C:\Users\$env:USERNAME\scoop\apps\ruby\current\bin\gem.cmd"

# Install required Ruby gems for WebKit build
Write-Host ":: Checking Ruby dependencies"
$RequiredGems = @("getoptlong")
foreach ($gem in $RequiredGems) {
    Write-Host "  Checking for gem: $gem"
    $gemCheck = & gem list $gem 2>&1
    if ($gemCheck -notlike "*$gem*") {
        Write-Host "  Installing missing gem: $gem"
        & gem install $gem --no-document
        if ($LASTEXITCODE -ne 0) {
            Write-Host "  Warning: Failed to install $gem, build may fail"
        }
    }
}

# Check for ccache
$CcacheLauncher = @()
$CcachePath = Get-Command ccache -ErrorAction SilentlyContinue
if ($CcachePath) {
    Write-Host ":: Found ccache at $($CcachePath.Source), enabling compiler cache"
    $CcacheLauncher = @(
        "-DCMAKE_C_COMPILER_LAUNCHER=ccache",
        "-DCMAKE_CXX_COMPILER_LAUNCHER=ccache"
    )
} else {
    Write-Host ":: ccache not found, building without compiler cache"
}

cmake -S . -B $WebKitBuild `
    -DPORT="JSCOnly" `
    -DCMAKE_SYSTEM_PROCESSOR=ARM64 `
    @CcacheLauncher `
    -DENABLE_STATIC_JSC=ON `
    -DALLOW_LINE_AND_COLUMN_NUMBER_IN_BUILTINS=ON `
    "-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}" `
    -DUSE_THIN_ARCHIVES=OFF `
    -DENABLE_JIT=ON `
    -DENABLE_DFG_JIT=ON `
    -DENABLE_FTL_JIT=ON `
    -DENABLE_WEBASSEMBLY_BBQJIT=ON `
    -DENABLE_WEBASSEMBLY_OMGJIT=ON `
    -DENABLE_SAMPLING_PROFILER=ON `
    "-DENABLE_WEBASSEMBLY=${WebAssemblyState}" `
    -DUSE_BUN_JSC_ADDITIONS=ON `
    -DUSE_BUN_EVENT_LOOP=ON `
    -DENABLE_BUN_SKIP_FAILING_ASSERTIONS=ON `
    -DUSE_SYSTEM_MALLOC=ON `
    "-DICU_ROOT=${ICU_STATIC_ROOT}" `
    "-DICU_LIBRARY=${ICU_STATIC_LIBRARY}" `
    "-DICU_INCLUDE_DIR=${ICU_STATIC_INCLUDE_DIR}" `
    "-DRuby_EXECUTABLE=${RubyPath}" `
    "-DCMAKE_C_COMPILER=clang-cl" `
    "-DCMAKE_CXX_COMPILER=clang-cl" `
    "-DCMAKE_C_FLAGS_RELEASE=/Zi /O2 /Ob2 /DNDEBUG /MT /clang:-fno-asynchronous-unwind-tables " `
    "-DCMAKE_CXX_FLAGS_RELEASE=/Zi /O2 /Ob2 /DNDEBUG /MT -Xclang -fno-c++-static-destructors /clang:-fno-asynchronous-unwind-tables " `
    "-DCMAKE_C_FLAGS_DEBUG=/Zi /FS /O0 /Ob0 /MTd " `
    "-DCMAKE_CXX_FLAGS_DEBUG=/Zi /FS /O0 /Ob0 /MTd -Xclang -fno-c++-static-destructors " `
    -DENABLE_REMOTE_INSPECTOR=ON `
    "-DCMAKE_MSVC_RUNTIME_LIBRARY=${CmakeMsvcRuntimeLibrary}" `
    @VcpkgToolchain `
    -G Ninja
# TODO: "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded" `
if ($LASTEXITCODE -ne 0) { throw "cmake failed with exit code $LASTEXITCODE" }

# Workaround for what is probably a CMake bug
$batFiles = Get-ChildItem -Path $WebKitBuild -Filter "*.bat" -File -Recurse
foreach ($file in $batFiles) {
    $content = Get-Content $file.FullName -Raw
    $newContent = $content -replace "(\|\| \(set FAIL_LINE=\d+& goto :ABORT\))", ""
    if ($content -ne $newContent) {
        Set-Content -Path $file.FullName -Value $newContent
        Write-Host ":: Patch $($file.FullName)"
    }
}

Write-Host ":: Building WebKit"
cmake --build $WebKitBuild --config Release --target jsc --verbose
if ($LASTEXITCODE -ne 0) { throw "cmake --build failed with exit code $LASTEXITCODE" }

Write-Host ":: Packaging ${output}"

# Dump the entire tree of files in $WebKitBuild to the console.
# This is useful for debugging.
Get-ChildItem -Recurse $WebKitBuild | Format-List -Force | Out-String | Write-Host

Remove-Item -Recurse -ErrorAction SilentlyContinue $output
$null = mkdir -ErrorAction SilentlyContinue $output
$null = mkdir -ErrorAction SilentlyContinue $output/include
$null = mkdir -ErrorAction SilentlyContinue $output/include/JavaScriptCore
$null = mkdir -ErrorAction SilentlyContinue $output/include/wtf

Copy-Item -Recurse $WebKitBuild/lib $output
Copy-Item -Recurse $WebKitBuild/bin $output

# If there's a lib64, also copy it.
if (Test-Path -Path $WebKitBuild/lib64) {
    Copy-Item -Recurse $WebKitBuild/lib64/* $output/lib
}

Copy-Item $WebKitBuild/cmakeconfig.h $output/include/cmakeconfig.h

if ($CMAKE_BUILD_TYPE -eq "Release") {
    Move-Item $ICU_STATIC_LIBRARY/icudt.lib $ICU_STATIC_LIBRARY/sicudt.lib
    Move-Item $ICU_STATIC_LIBRARY/icutu.lib $ICU_STATIC_LIBRARY/sicutu.lib
    Move-Item $ICU_STATIC_LIBRARY/icuio.lib $ICU_STATIC_LIBRARY/sicuio.lib
    Move-Item $ICU_STATIC_LIBRARY/icuin.lib $ICU_STATIC_LIBRARY/sicuin.lib
    Move-Item $ICU_STATIC_LIBRARY/icuuc.lib $ICU_STATIC_LIBRARY/sicuuc.lib
} elseif ($CMAKE_BUILD_TYPE -eq "Debug") {
    Move-Item $ICU_STATIC_LIBRARY/icudt.lib $ICU_STATIC_LIBRARY/sicudtd.lib
    Move-Item $ICU_STATIC_LIBRARY/icutu.lib $ICU_STATIC_LIBRARY/sicutud.lib
    Move-Item $ICU_STATIC_LIBRARY/icuio.lib $ICU_STATIC_LIBRARY/sicuiod.lib
    Move-Item $ICU_STATIC_LIBRARY/icuin.lib $ICU_STATIC_LIBRARY/sicuind.lib
    Move-Item $ICU_STATIC_LIBRARY/icuuc.lib $ICU_STATIC_LIBRARY/sicuucd.lib
}

Add-Content -Path $output/include/cmakeconfig.h -Value "`#define BUN_WEBKIT_VERSION `"$BUN_WEBKIT_VERSION`""

Copy-Item -r -Force $WebKitBuild/JavaScriptCore/DerivedSources/* $output/include/JavaScriptCore
Copy-Item -r -Force $WebKitBuild/JavaScriptCore/Headers/JavaScriptCore/* $output/include/JavaScriptCore/
Copy-Item -r -Force $WebKitBuild/JavaScriptCore/PrivateHeaders/JavaScriptCore/* $output/include/JavaScriptCore/
# Recursively copy all the .h files in DerivedSources to the root of include/JavaScriptCore, preserving the basename only.
Copy-Item -r -Force $WebKitBuild/JavaScriptCore/DerivedSources/*.h $output/include/JavaScriptCore/
Copy-Item -r -Force $WebKitBuild/JavaScriptCore/DerivedSources/*/*.h $output/include/JavaScriptCore/

# Recursively copy all the .json files in DerivedSources to the root of the output directory, preserving the basename only.
Copy-Item -r -Force $WebKitBuild/JavaScriptCore/DerivedSources/*.json $output/

Copy-Item -r $WebKitBuild/WTF/DerivedSources/* $output/include/wtf/
Copy-Item -r $WebKitBuild/WTF/Headers/wtf/* $output/include/wtf/

# Copy bmalloc headers if they exist (libpas support)
if (Test-Path -Path $WebKitBuild/bmalloc) {
    $null = mkdir -ErrorAction SilentlyContinue $output/include/bmalloc
    Copy-Item -r $WebKitBuild/bmalloc/Headers/bmalloc/* $output/include/bmalloc/
}

(Get-Content -Path $output/include/JavaScriptCore/JSValueInternal.h) `
    -replace "#import <JavaScriptCore/JSValuePrivate.h>", "#include <JavaScriptCore/JSValuePrivate.h>" `
| Set-Content -Path $output/include/JavaScriptCore/JSValueInternal.h

Copy-Item -r $ICU_STATIC_INCLUDE_DIR/* $output/include/
Copy-Item -r $ICU_STATIC_LIBRARY/* $output/lib/

$packageJsonContent = @{
    name       = $env:PACKAGE_JSON_LABEL
    version    = "0.0.1-$env:GITHUB_SHA"
    os         = @("windows")
    cpu        = @($env:PACKAGE_JSON_ARCH)
    repository = "https://github.com/$($env:GITHUB_REPOSITORY)"
} | ConvertTo-Json -Depth 2
Out-File -FilePath $output/package.json -InputObject $packageJsonContent

tar -cz -f "${output}.tar.gz" "${output}"
if ($LASTEXITCODE -ne 0) { throw "tar failed with exit code $LASTEXITCODE" }

Write-Host "-> ${output}.tar.gz"
