[CmdletBinding()]
param(
  [ValidateSet("User", "Process")]
  [string]$Scope = "User",
  [switch]$SetGhTokenAlso,
  [switch]$FromClipboard
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$plainToken = $null

function Test-LooksLikeGitHubToken {
  param([string]$Value)

  if ([string]::IsNullOrWhiteSpace($Value)) {
    return $false
  }

  return $Value -match '^(github_pat_|gh[pousr]_)'
}

function Read-MaskedToken {
  param([string]$Prompt)

  Write-Host -NoNewline ($Prompt + ": ")
  $buffer = New-Object System.Collections.Generic.List[char]

  while ($true) {
    $key = [System.Console]::ReadKey($true)

    if ($key.Key -eq [ConsoleKey]::Enter) {
      Write-Host
      break
    }

    if ($key.Key -eq [ConsoleKey]::Backspace) {
      if ($buffer.Count -gt 0) {
        $buffer.RemoveAt($buffer.Count - 1)
        Write-Host -NoNewline "`b `b"
      }
      continue
    }

    if ([char]::IsControl($key.KeyChar)) {
      continue
    }

    $buffer.Add($key.KeyChar)
    Write-Host -NoNewline "*"
  }

  return -join $buffer.ToArray()
}

try {
  if ($FromClipboard) {
    $clipboardValue = Get-Clipboard -Raw
    if ($null -eq $clipboardValue) {
      $clipboardValue = ""
    }
    $plainToken = ([regex]::Replace($clipboardValue, "\s+", "")).Trim()
  } else {
    $plainToken = ([regex]::Replace((Read-MaskedToken -Prompt "Paste the GitHub token"), "\s+", "")).Trim()
  }

  if ([string]::IsNullOrWhiteSpace($plainToken) -or $plainToken.Length -lt 20 -or -not (Test-LooksLikeGitHubToken -Value $plainToken)) {
    throw "The entered value does not look like a GitHub token. Copy the PAT itself from GitHub and try again."
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
  if ($plainToken) {
    $plainToken = $null
  }
}
