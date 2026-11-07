# Builds Evil Dead: Hail to the King for Windows from your own disc image.
# Run it with build.bat (double-click). It:
#   1. asks for your disc's .cue file (Redump .cue + .bin set),
#   2. downloads a compiler and build tools into the .tools folder (about 400 MB, once),
#   3. reads the game's program from your disc and translates it to C,
#   4. compiles it and puts the finished game in the pc-build folder.
# Nothing from the game is downloaded or included in this project.
param([string]$Cue)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'          # makes Invoke-WebRequest much faster
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$Root  = Split-Path -Parent $MyInvocation.MyCommand.Path
$Game  = Join-Path $Root 'games\hail-to-the-king'
$Tools = Join-Path $Root '.tools'
$Out   = Join-Path $Root 'pc-build'
New-Item -ItemType Directory -Force -Path $Tools | Out-Null

function Step($n, $text) { Write-Host ""; Write-Host "== $n/5  $text" -ForegroundColor Cyan }

# ---------- 1. the disc ----------
Step 1 'Finding your disc'
if (-not $Cue) {
    $near = @($Root, (Split-Path $Root -Parent)) | Where-Object { $_ } |
            ForEach-Object { Get-ChildItem -Path $_ -Filter *.cue -Recurse -Depth 2 -ErrorAction SilentlyContinue } |
            Where-Object { $_.Name -like '*Hail to the King*' } | Select-Object -First 1
    if ($near) { $Cue = $near.FullName; Write-Host "Found $Cue" }
}
if (-not $Cue) {
    Add-Type -AssemblyName System.Windows.Forms
    $dlg = New-Object System.Windows.Forms.OpenFileDialog
    $dlg.Title = 'Pick your Evil Dead: Hail to the King disc (.cue)'
    $dlg.Filter = 'Disc image (*.cue)|*.cue'
    if ($dlg.ShowDialog() -ne 'OK') { throw 'No disc picked.' }
    $Cue = $dlg.FileName
}
$Cue = (Resolve-Path $Cue).Path

# ---------- 2. build tools ----------
Step 2 'Getting the build tools (first time only)'
$downloads = @(
    @{ Name = 'llvm-mingw'; Url = 'https://github.com/mstorsjo/llvm-mingw/releases/download/20241217/llvm-mingw-20241217-ucrt-x86_64.zip'; Check = 'bin\x86_64-w64-mingw32-clang.exe' },
    @{ Name = 'cmake';      Url = 'https://github.com/Kitware/CMake/releases/download/v3.30.5/cmake-3.30.5-windows-x86_64.zip';        Check = 'bin\cmake.exe' },
    @{ Name = 'ninja';      Url = 'https://github.com/ninja-build/ninja/releases/download/v1.12.1/ninja-win.zip';                       Check = 'ninja.exe' },
    @{ Name = 'python';     Url = 'https://www.python.org/ftp/python/3.12.7/python-3.12.7-embed-amd64.zip';                            Check = 'python.exe' },
    @{ Name = 'sdl2';       Url = 'https://github.com/libsdl-org/SDL/releases/download/release-2.30.9/SDL2-devel-2.30.9-mingw.zip';    Check = 'x86_64-w64-mingw32\lib\libSDL2.dll.a' }
)
$dirs = @{}
foreach ($d in $downloads) {
    $dest = Join-Path $Tools $d.Name
    # zips that hold a single top folder: find the folder that contains the file we need
    $found = $null
    if (Test-Path $dest) {
        $found = Get-ChildItem -Path $dest -Directory -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName } |
                 Where-Object { Test-Path (Join-Path $_ $d.Check) } | Select-Object -First 1
        if (-not $found -and (Test-Path (Join-Path $dest $d.Check))) { $found = $dest }
    }
    if (-not $found) {
        Write-Host "Downloading $($d.Name)..."
        $zip = Join-Path $Tools "$($d.Name).zip"
        Invoke-WebRequest -Uri $d.Url -OutFile $zip -UseBasicParsing
        if (Test-Path $dest) { Remove-Item -Recurse -Force $dest }
        New-Item -ItemType Directory -Force -Path $dest | Out-Null
        & tar.exe -xf $zip -C $dest
        if ($LASTEXITCODE -ne 0) { Expand-Archive -Path $zip -DestinationPath $dest -Force }
        Remove-Item $zip
        $found = Get-ChildItem -Path $dest -Directory | ForEach-Object { $_.FullName } |
                 Where-Object { Test-Path (Join-Path $_ $d.Check) } | Select-Object -First 1
        if (-not $found -and (Test-Path (Join-Path $dest $d.Check))) { $found = $dest }
        if (-not $found) { throw "Download of $($d.Name) did not contain $($d.Check)" }
    }
    $dirs[$d.Name] = $found
}
# the embedded Python ignores the script's folder unless its ._pth file is removed
Get-ChildItem -Path $dirs['python'] -Filter 'python*._pth' | Remove-Item -Force
$py    = Join-Path $dirs['python'] 'python.exe'
$llvm  = $dirs['llvm-mingw']
$cmake = Join-Path $dirs['cmake'] 'bin\cmake.exe'
$ninja = Join-Path $dirs['ninja'] 'ninja.exe'
$sdl   = Join-Path $dirs['sdl2'] 'x86_64-w64-mingw32'

# ---------- 3. program from the disc -> C ----------
Step 3 'Reading the game program from your disc'
& $py (Join-Path $Game 'tools\extract_disc.py') $Cue (Join-Path $Game 'orig')
if ($LASTEXITCODE -ne 0) { throw 'Could not read the disc.' }
& $py (Join-Path $Game 'tools\make_overlay.py') (Join-Path $Game 'orig\1ST_READ.BIN')
if ($LASTEXITCODE -ne 0) { throw 'Could not rebuild the start-up code.' }

Step 4 'Translating the program to C (a few minutes)'
Push-Location (Join-Path $Game 'tools')
try { & $py recomp.py ..\orig\1ST_READ.BIN ..\build\gen | Out-Null; if ($LASTEXITCODE -ne 0) { throw 'Translation failed.' } }
finally { Pop-Location }

# ---------- 5. compile + package ----------
Step 5 'Compiling (10-20 minutes the first time)'
$env:PATH = "$llvm\bin;$(Split-Path $ninja);$env:PATH"
$bdir = Join-Path $Game 'build\win'
$cc = (Join-Path $llvm 'bin\x86_64-w64-mingw32-clang.exe') -replace '\\', '/'
& $cmake -S $Game -B $bdir -G Ninja "-DCMAKE_MAKE_PROGRAM=$($ninja -replace '\\','/')" "-DCMAKE_C_COMPILER=$cc" `
    -DCMAKE_BUILD_TYPE=Release -DPLATFORM=sdl "-DEXT_INC=$(($sdl + '\include') -replace '\\','/')" `
    "-DSDL2_LIB=$(($sdl + '\lib\libSDL2.dll.a') -replace '\\','/')" -DCMAKE_EXE_LINKER_FLAGS=-static
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }
& $ninja -C $bdir
if ($LASTEXITCODE -ne 0) { throw 'Compiling failed.' }

New-Item -ItemType Directory -Force -Path $Out | Out-Null
Copy-Item (Join-Path $bdir 'hail-to-the-king.exe') $Out -Force
Copy-Item (Join-Path $sdl 'bin\SDL2.dll') $Out -Force
Copy-Item (Join-Path $Game 'dist\README.txt') $Out -Force
New-Item -ItemType Directory -Force -Path (Join-Path $Out 'textures') | Out-Null
Copy-Item (Join-Path $Game 'textures\*') (Join-Path $Out 'textures') -Recurse -Force
Copy-Item (Join-Path $Root 'LICENSE') (Join-Path $Out 'LICENSE.txt') -Force
$bats = @{ 'play.bat' = ''; 'options.bat' = ' --options'; 'launcher.bat' = ' --launcher' }
foreach ($b in $bats.Keys) {
    $body = "@echo off`r`ncd /d `"%~dp0`"`r`nhail-to-the-king.exe `"$Cue`"$($bats[$b]) 2> log.txt`r`n"
    Set-Content -Path (Join-Path $Out $b) -Value $body -Encoding ASCII -NoNewline
}
Write-Host ""
Write-Host "Done. Your game is in $Out - start launcher.bat." -ForegroundColor Green
