param([string]$DataDir = (Join-Path $PSScriptRoot '..\output\data'))
$ErrorActionPreference = 'Stop'
$resource = Join-Path $PSScriptRoot '..\resource\lexicons\taiwan'
$files = @('wisdom_taiwan.dict.yaml', 'wisdom_taiwan.NOTICE.txt', 'wisdom_taiwan.manifest.json')
foreach ($name in $files) {
  if (-not (Test-Path -LiteralPath (Join-Path $resource $name) -PathType Leaf)) {
    throw "Missing bundled vocabulary resource: $name"
  }
}
$manifest = Get-Content -LiteralPath (Join-Path $resource 'wisdom_taiwan.manifest.json') -Raw | ConvertFrom-Json
$stream = [IO.File]::OpenRead((Join-Path $resource 'wisdom_taiwan.dict.yaml'))
$sha = [Security.Cryptography.SHA256]::Create()
try { $hash = [BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-', '') }
finally { $stream.Dispose(); $sha.Dispose() }
if ($hash -ne $manifest.sha256) { throw 'Bundled vocabulary checksum mismatch' }
New-Item -ItemType Directory -Force -Path $DataDir | Out-Null
foreach ($name in $files) {
  Copy-Item -LiteralPath (Join-Path $resource $name) -Destination (Join-Path $DataDir $name) -Force
}
Write-Output "Staged Taiwan vocabulary: $($manifest.words) words with attribution and manifest."
