[CmdletBinding()]
param(
  [ValidateSet("User", "Process")]
  [string]$Scope = "User",
  [switch]$SetGhTokenAlso
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$secureToken = Read-Host "Paste the GitHub token" -AsSecureString
$tokenPtr = [IntPtr]::Zero

try {
  $tokenPtr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($secureToken)
  $plainToken = [Runtime.InteropServices.Marshal]::PtrToStringBSTR($tokenPtr).Trim()

  if ([string]::IsNullOrWhiteSpace($plainToken)) {
    throw "No token was entered."
  }

  switch ($Scope) {
    "Process" {
      $env:GITHUB_TOKEN = $plainToken
      if ($SetGhTokenAlso) {
        $env:GH_TOKEN = $plainToken
      }
    }
    "User" {
      [Environment]::SetEnvironmentVariable("GITHUB_TOKEN", $plainToken, "User")
      if ($SetGhTokenAlso) {
        [Environment]::SetEnvironmentVariable("GH_TOKEN", $plainToken, "User")
      }
    }
  }

  Write-Host ("GitHub token stored in scope: " + $Scope)
  if ($SetGhTokenAlso) {
    Write-Host "GH_TOKEN was updated too."
  }
} finally {
  if ($tokenPtr -ne [IntPtr]::Zero) {
    [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($tokenPtr)
  }
}
