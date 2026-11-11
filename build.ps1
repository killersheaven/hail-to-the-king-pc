# Builds Evil Dead: Hail to the King for Windows (build.bat) or Android (build-android.bat)
# from your own disc image. It:
#   1. asks for your disc's .cue file (Redump .cue + .bin set),
#   2. downloads a compiler and build tools into the .tools folder (about 400 MB, once),
#   3. reads the game's program from your disc and translates it to C,
#   4. compiles it and puts the finished game in the pc-build folder
#      (Android: an .apk in the android-build folder, for your phone).
# Nothing from the game is downloaded or included in this project.
param([string]$Cue, [switch]$Android)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'          # makes Invoke-WebRequest much faster
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$Root  = Split-Path -Parent $MyInvocation.MyCommand.Path
$Game  = Join-Path $Root 'games\hail-to-the-king'
$Tools = Join-Path $Root '.tools'
$Out   = Join-Path $Root $(if ($Android) { 'android-build' } else { 'pc-build' })
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
Step 2 $(if ($Android) { 'Getting the build tools, Java and Android tools (first time only, about 3 GB)' } else { 'Getting the build tools (first time only)' })
$downloads = @(
    @{ Name = 'llvm-mingw'; Url = 'https://github.com/mstorsjo/llvm-mingw/releases/download/20241217/llvm-mingw-20241217-ucrt-x86_64.zip'; Check = 'bin\x86_64-w64-mingw32-clang.exe' },
    @{ Name = 'cmake';      Url = 'https://github.com/Kitware/CMake/releases/download/v3.30.5/cmake-3.30.5-windows-x86_64.zip';        Check = 'bin\cmake.exe' },
    @{ Name = 'ninja';      Url = 'https://github.com/ninja-build/ninja/releases/download/v1.12.1/ninja-win.zip';                       Check = 'ninja.exe' },
    @{ Name = 'python';     Url = 'https://www.python.org/ftp/python/3.12.7/python-3.12.7-embed-amd64.zip';                            Check = 'python.exe' },
    @{ Name = 'sdl2';       Url = 'https://github.com/libsdl-org/SDL/releases/download/release-2.30.9/SDL2-devel-2.30.9-mingw.zip';    Check = 'x86_64-w64-mingw32\lib\libSDL2.dll.a' }
)
if ($Android) {
    $downloads = @($downloads | Where-Object { $_.Name -ne 'llvm-mingw' -and $_.Name -ne 'sdl2' }) + @(
        @{ Name = 'jdk';           Url = 'https://github.com/adoptium/temurin17-binaries/releases/download/jdk-17.0.13%2B11/OpenJDK17U-jdk_x64_windows_hotspot_17.0.13_11.zip'; Check = 'bin\javac.exe' },
        @{ Name = 'cmdline-tools'; Url = 'https://dl.google.com/android/repository/commandlinetools-win-11076708_latest.zip'; Check = 'bin\sdkmanager.bat' },
        @{ Name = 'sdl2-src';      Url = 'https://github.com/libsdl-org/SDL/releases/download/release-2.30.9/SDL2-2.30.9.zip'; Check = 'android-project\app\src\main\java\org\libsdl\app\SDLActivity.java' }
    )
}
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
$cmake = Join-Path $dirs['cmake'] 'bin\cmake.exe'
$ninja = Join-Path $dirs['ninja'] 'ninja.exe'
if (-not $Android) { $llvm = $dirs['llvm-mingw']; $sdl = Join-Path $dirs['sdl2'] 'x86_64-w64-mingw32' }
else {
    # Android SDK pieces through Google's sdkmanager: the platform library, build tools and the NDK
    $env:JAVA_HOME = $dirs['jdk']; $env:PATH = "$($dirs['jdk'])\bin;$env:PATH"
    $sdk = Join-Path $Tools 'android-sdk'; $ndkVer = '27.2.12479018'
    $ndk = Join-Path $sdk "ndk\$ndkVer"; $bt = Join-Path $sdk 'build-tools\34.0.0'; $ajar = Join-Path $sdk 'platforms\android-34\android.jar'
    if (-not ((Test-Path (Join-Path $ndk 'build\cmake\android.toolchain.cmake')) -and (Test-Path (Join-Path $bt 'aapt2.exe')) -and (Test-Path $ajar))) {
        $sm = Join-Path $dirs['cmdline-tools'] 'bin\sdkmanager.bat'
        Write-Host 'Accepting the Android SDK licences and downloading the NDK (this is the big one)...'
        ("y`n" * 40) | & $sm "--sdk_root=$sdk" --licenses | Out-Null
        & $sm "--sdk_root=$sdk" 'platforms;android-34' 'build-tools;34.0.0' "ndk;$ndkVer"
        if ($LASTEXITCODE -ne 0) { throw 'The Android tools could not be installed.' }
    }
}

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

# ---------- 5 (Android). compile + package an APK ----------
if ($Android) {
    Step 5 'Compiling for Android and packaging the app (15-30 minutes the first time)'
    $env:PATH = "$(Split-Path $ninja);$env:PATH"
    $adir = Join-Path $Game 'build\android'
    $fw = { param($p) $p -replace '\\', '/' }
    & $cmake -S (Join-Path $Game 'android') -B (Join-Path $adir 'native') -G Ninja "-DCMAKE_MAKE_PROGRAM=$(& $fw $ninja)" `
        "-DCMAKE_TOOLCHAIN_FILE=$(& $fw (Join-Path $ndk 'build\cmake\android.toolchain.cmake'))" -DANDROID_ABI=arm64-v8a `
        -DANDROID_PLATFORM=android-24 -DCMAKE_BUILD_TYPE=Release "-DSDL_SRC=$(& $fw $dirs['sdl2-src'])"
    if ($LASTEXITCODE -ne 0) { throw 'CMake configuration (Android) failed.' }
    & $ninja -C (Join-Path $adir 'native')
    if ($LASTEXITCODE -ne 0) { throw 'Compiling (Android) failed.' }
    $libmain = Get-ChildItem (Join-Path $adir 'native') -Recurse -Filter 'libmain.so' | Select-Object -First 1
    $libsdl  = Get-ChildItem (Join-Path $adir 'native') -Recurse -Filter 'libSDL2.so' | Select-Object -First 1
    if (-not $libmain -or -not $libsdl) { throw 'The native libraries were not built.' }

    # Java: SDL's activity classes + ours -> classes.dex
    $cls = Join-Path $adir 'classes'; if (Test-Path $cls) { Remove-Item -Recurse -Force $cls }; New-Item -ItemType Directory -Path $cls | Out-Null
    $javas = @(Get-ChildItem (Join-Path $dirs['sdl2-src'] 'android-project\app\src\main\java') -Recurse -Filter *.java) +
             @(Get-ChildItem (Join-Path $Game 'android\java') -Recurse -Filter *.java) | ForEach-Object { '"' + ($_.FullName -replace '\\','/') + '"' }
    $argf = Join-Path $adir 'javac-files.txt'; Set-Content -Path $argf -Value $javas -Encoding ASCII
    & javac -nowarn -source 8 -target 8 -encoding UTF-8 -classpath $ajar -d $cls "@$argf"
    if ($LASTEXITCODE -ne 0) { throw 'Compiling the Java part failed.' }
    $jar = Join-Path $adir 'classes.jar'; & jar cf $jar -C $cls .
    $dex = Join-Path $adir 'dex'; if (Test-Path $dex) { Remove-Item -Recurse -Force $dex }; New-Item -ItemType Directory -Path $dex | Out-Null
    & (Join-Path $bt 'd8.bat') --release --min-api 24 --lib $ajar --output $dex $jar
    if ($LASTEXITCODE -ne 0) { throw 'd8 failed.' }

    # resources, manifest and the bundled textures -> APK; then code and libraries
    $assets = Join-Path $adir 'assets'; if (Test-Path $assets) { Remove-Item -Recurse -Force $assets }
    New-Item -ItemType Directory -Force -Path (Join-Path $assets 'textures') | Out-Null
    Copy-Item (Join-Path $Game 'textures\*') (Join-Path $assets 'textures') -Recurse -Force
    $aapt = Join-Path $bt 'aapt2.exe'; $resz = Join-Path $adir 'res.zip'
    & $aapt compile --dir (Join-Path $Game 'android\res') -o $resz
    if ($LASTEXITCODE -ne 0) { throw 'aapt2 compile failed.' }
    $base = Join-Path $adir 'base.apk'
    & $aapt link -o $base -I $ajar --manifest (Join-Path $Game 'android\AndroidManifest.xml') --min-sdk-version 24 `
        --target-sdk-version 34 -A $assets $resz
    if ($LASTEXITCODE -ne 0) { throw 'aapt2 link failed.' }
    $raw = Join-Path $adir 'unsigned.apk'
    & $py (Join-Path $Game 'tools\apk_add.py') $base $raw "classes.dex=$(Join-Path $dex 'classes.dex')" `
        "lib/arm64-v8a/libmain.so=$($libmain.FullName)" "lib/arm64-v8a/libSDL2.so=$($libsdl.FullName)"
    if ($LASTEXITCODE -ne 0) { throw 'Packaging failed.' }
    $aligned = Join-Path $adir 'aligned.apk'
    & (Join-Path $bt 'zipalign.exe') -f -p 4 $raw $aligned
    if ($LASTEXITCODE -ne 0) { throw 'zipalign failed.' }
    $ks = Join-Path $Tools 'android-signing.keystore'     # your own signing key, made once and kept for updates
    if (-not (Test-Path $ks)) {
        & keytool -genkeypair -keystore $ks -alias httk -keyalg RSA -keysize 2048 -validity 10000 `
            -storepass hailtotheking -keypass hailtotheking -dname 'CN=Hail to the King PC build'
        if ($LASTEXITCODE -ne 0) { throw 'keytool failed.' }
    }
    New-Item -ItemType Directory -Force -Path $Out | Out-Null
    $apk = Join-Path $Out 'hail-to-the-king.apk'
    & (Join-Path $bt 'apksigner.bat') sign --ks $ks --ks-pass pass:hailtotheking --key-pass pass:hailtotheking --out $apk $aligned
    if ($LASTEXITCODE -ne 0) { throw 'apksigner failed.' }
    Copy-Item (Join-Path $Game 'dist\README-android.txt') $Out -Force
    Write-Host ""
    Write-Host "Done. Your app is $apk - see README-android.txt in that folder to install it." -ForegroundColor Green
    return
}

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
