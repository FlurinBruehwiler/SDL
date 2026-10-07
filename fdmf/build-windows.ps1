# Builds the win-x64 SDL3.dll that FDMF ships instead of the one from the SDL3-CS.Native package.
# This branch is based on the commit that package's DLL was built from, so the C# bindings match.
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
$build = Join-Path $root 'build-fdmf'

cmake -S $root -B $build -G 'Visual Studio 17 2022' -A x64 -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF
cmake --build $build --config Release

New-Item -ItemType Directory -Force (Join-Path $PSScriptRoot 'win-x64') | Out-Null
Copy-Item (Join-Path $build 'Release\SDL3.dll') (Join-Path $PSScriptRoot 'win-x64\SDL3.dll') -Force
