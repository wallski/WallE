# Wall-E Release Packaging Script
# This script automates the creation of a 'ready-to-upload' zip folder.

$v = "v1.0.0"
$releaseDir = "WallE-$v"

Write-Host "--- Packaging Wall-E $v ---" -ForegroundColor Cyan

# 1. Build the project in Release mode
Write-Host "Building project..."
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" "WallE.vcxproj" /p:Configuration=Release /p:Platform=x64 /nologo /v:minimal

if ($LASTEXITCODE -ne 0) {
    Write-Host "Build failed! Aborting." -ForegroundColor Red
    exit 1
}

# 2. Setup clean directory
if (Test-Path $releaseDir) { Remove-Item -Recurse -Force $releaseDir }
New-Item -ItemType Directory -Path $releaseDir

# 3. Copy binaries
Write-Host "Copying files..."
Copy-Item "x64\Release\WallE.exe" "$releaseDir\"
Copy-Item -Recurse "platform-tools" "$releaseDir\"
Copy-Item "LICENSE" "$releaseDir\"
Copy-Item "README.md" "$releaseDir\"

# 4. Clean up unnecessary files in platform-tools (keep only essentials)
$keepList = @("adb.exe", "AdbWinApi.dll", "AdbWinUsbApi.dll")
Get-ChildItem "$releaseDir\platform-tools" | Where-Object { $_.Name -notin $keepList } | Remove-Item -Force

Write-Host "--- DONE! ---" -ForegroundColor Green
Write-Host "Your release is ready in: $releaseDir"
Write-Host "You can now zip this folder and upload it to GitHub Releases."
