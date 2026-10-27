param([Parameter(Mandatory=$true)][string]$Link, [Parameter(Mandatory=$true)][string]$Target)
$ErrorActionPreference = 'Stop'
New-Item -ItemType Junction -Path $Link -Target $Target | Out-Null
