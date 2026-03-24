[CmdletBinding()]
param(
  [string]$Repo = "alejomazabuel/xemu",
  [string]$Token = "",
  [ValidateSet("None", "Process", "User")]
  [string]$PersistScope = "Process",
  [switch]$SetGhTokenAlso,
  [string]$OutputDirectory = "build/github-auth"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Resolve-RepositoryParts {
  param([string]$Repository)

  if ($Repository -notmatch "^(?<owner>[^/]+)/(?<name>[^/]+)$") {
    throw "Repo must use the format owner/name. Received: $Repository"
  }

  return @{
    Owner = $Matches.owner
    Name = $Matches.name
  }
}

function Resolve-Token {
  param([string]$ExplicitToken)

  function Normalize-TokenValue {
    param([string]$Value)

    if ([string]::IsNullOrWhiteSpace($Value)) {
      return ""
    }

    return ([regex]::Replace($Value, "\s+", "")).Trim()
  }

  function New-TokenInfo {
    param(
      [string]$Value,
      [string]$Source
    )

    $normalizedValue = Normalize-TokenValue -Value $Value
    if ([string]::IsNullOrWhiteSpace($normalizedValue)) {
      return $null
    }

    return @{
      Value = $normalizedValue
      Source = $Source
    }
  }

  if (-not [string]::IsNullOrWhiteSpace($ExplicitToken)) {
    return (New-TokenInfo -Value $ExplicitToken -Source "argument")
  }

  $userGitHubToken = [Environment]::GetEnvironmentVariable("GITHUB_TOKEN", "User")
  if (-not [string]::IsNullOrWhiteSpace($userGitHubToken)) {
    return (New-TokenInfo -Value $userGitHubToken -Source "user:GITHUB_TOKEN")
  }

  $userGhToken = [Environment]::GetEnvironmentVariable("GH_TOKEN", "User")
  if (-not [string]::IsNullOrWhiteSpace($userGhToken)) {
    return (New-TokenInfo -Value $userGhToken -Source "user:GH_TOKEN")
  }

  if (-not [string]::IsNullOrWhiteSpace($env:GITHUB_TOKEN)) {
    return (New-TokenInfo -Value $env:GITHUB_TOKEN -Source "env:GITHUB_TOKEN")
  }

  if (-not [string]::IsNullOrWhiteSpace($env:GH_TOKEN)) {
    return (New-TokenInfo -Value $env:GH_TOKEN -Source "env:GH_TOKEN")
  }

  throw "Provide -Token or set GITHUB_TOKEN / GH_TOKEN before running this script."
}

function Assert-TokenLooksValid {
  param(
    [Parameter(Mandatory = $true)][hashtable]$TokenInfo
  )

  $looksLikeGitHubToken = $TokenInfo.Value -match '^(github_pat_|gh[pousr]_)'
  if ($TokenInfo.Value.Length -lt 20 -or -not $looksLikeGitHubToken) {
    $prefix = $TokenInfo.Value.Substring(0, [Math]::Min(8, $TokenInfo.Value.Length))
    throw "The value loaded from $($TokenInfo.Source) does not look like a GitHub token. Current prefix: '$prefix'. Re-run set-github-token.cmd and copy only the PAT."
  }
}

function Invoke-GitHubApi {
  param(
    [Parameter(Mandatory = $true)][string]$Method,
    [Parameter(Mandatory = $true)][string]$Uri,
    [Parameter(Mandatory = $true)][string]$BearerToken,
    [switch]$Raw
  )

  $headers = @{
    Authorization = "Bearer $BearerToken"
    Accept = "application/vnd.github+json"
    "X-GitHub-Api-Version" = "2022-11-28"
    "User-Agent" = "x1box-github-token-refresh"
  }

  if ($Raw) {
    return Invoke-WebRequest -UseBasicParsing -Method $Method -Uri $Uri -Headers $headers
  }

  return Invoke-RestMethod -Method $Method -Uri $Uri -Headers $headers
}

function Persist-Token {
  param(
    [string]$BearerToken,
    [string]$Scope,
    [bool]$AlsoSetGhToken
  )

  switch ($Scope) {
    "None" { }
    "Process" {
      $env:GITHUB_TOKEN = $BearerToken
      if ($AlsoSetGhToken) {
        $env:GH_TOKEN = $BearerToken
      }
    }
    "User" {
      [Environment]::SetEnvironmentVariable("GITHUB_TOKEN", $BearerToken, "User")
      if ($AlsoSetGhToken) {
        [Environment]::SetEnvironmentVariable("GH_TOKEN", $BearerToken, "User")
      }
    }
  }
}

$tokenInfo = Resolve-Token -ExplicitToken $Token
Assert-TokenLooksValid -TokenInfo $tokenInfo
$repoParts = Resolve-RepositoryParts -Repository $Repo
$repoApiRoot = "https://api.github.com/repos/$($repoParts.Owner)/$($repoParts.Name)"

$viewerResponse = Invoke-GitHubApi -Method GET -Uri "https://api.github.com/user" -BearerToken $tokenInfo.Value
$repoResponse = Invoke-GitHubApi -Method GET -Uri $repoApiRoot -BearerToken $tokenInfo.Value
$workflowResponse = Invoke-GitHubApi -Method GET -Uri "$repoApiRoot/actions/workflows?per_page=100" -BearerToken $tokenInfo.Value
$rawRepoResponse = Invoke-GitHubApi -Method GET -Uri $repoApiRoot -BearerToken $tokenInfo.Value -Raw

$oauthScopes = ""
if ($rawRepoResponse.Headers.ContainsKey("X-OAuth-Scopes")) {
  $oauthScopes = [string]$rawRepoResponse.Headers["X-OAuth-Scopes"]
}

Persist-Token -BearerToken $tokenInfo.Value -Scope $PersistScope -AlsoSetGhToken:$SetGhTokenAlso

$resolvedOutput = if ([System.IO.Path]::IsPathRooted($OutputDirectory)) {
  $OutputDirectory
} else {
  Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) $OutputDirectory
}

New-Item -ItemType Directory -Force -Path $resolvedOutput | Out-Null
$summaryPath = Join-Path $resolvedOutput "github-token-status.json"

$summary = [ordered]@{
  checked_at_utc = (Get-Date).ToUniversalTime().ToString("o")
  repository = $Repo
  token_source = $tokenInfo.Source
  persist_scope = $PersistScope
  gh_token_also_set = [bool]$SetGhTokenAlso
  github_login = [string]$viewerResponse.login
  github_name = [string]$viewerResponse.name
  repo_access = @{
    can_read = $true
    permissions = $repoResponse.permissions
    private = [bool]$repoResponse.private
    default_branch = [string]$repoResponse.default_branch
  }
  actions_access = @{
    workflow_count = @($workflowResponse.workflows).Count
    workflow_names = @($workflowResponse.workflows | Select-Object -ExpandProperty name)
  }
  oauth_scopes_header = $oauthScopes
}

$summary | ConvertTo-Json -Depth 8 | Set-Content -Path $summaryPath -Encoding UTF8

Write-Host ("GitHub login: " + $summary.github_login)
Write-Host ("Repository: " + $Repo)
Write-Host ("Workflow count visible: " + $summary.actions_access.workflow_count)
Write-Host ("Token source: " + $tokenInfo.Source)
Write-Host ("Persisted to: " + $PersistScope)
Write-Host ("Summary written to: " + $summaryPath)
