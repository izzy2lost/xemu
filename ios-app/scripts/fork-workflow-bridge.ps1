[CmdletBinding()]
param(
  [ValidateSet("dispatch", "watch", "download", "full", "status", "sync")]
  [string]$Mode = "full",

  [string]$Repo = "alejomazabuel/xemu",
  [string]$Workflow = "build-ios-app.yml",
  [string]$Ref = "master",
  [string]$SimDestination = "platform=iOS Simulator,name=iPhone 16,OS=latest",
  [bool]$RunDeviceBuild = $true,
  [string]$ArtifactName = "x1box-ios-ci",
  [string]$OutputDirectory = "build/fork-ios-ci",
  [string]$StatusFileName = "latest-status.json",
  [int]$PollSeconds = 15,
  [int]$TimeoutMinutes = 45,
  [Nullable[int64]]$RunId = $null
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Get-RepositoryParts {
  param([string]$Repository)

  if ($Repository -notmatch "^(?<owner>[^/]+)/(?<name>[^/]+)$") {
    throw "Repo must use the format owner/name. Received: $Repository"
  }

  return @{
    Owner = $Matches.owner
    Name = $Matches.name
  }
}

function Get-GitHubToken {
  if ($env:GITHUB_TOKEN) {
    return $env:GITHUB_TOKEN
  }
  if ($env:GH_TOKEN) {
    return $env:GH_TOKEN
  }
  throw "Set GITHUB_TOKEN or GH_TOKEN with repo/actions permissions before using this bridge."
}

function Invoke-GitHubApi {
  param(
    [Parameter(Mandatory = $true)][string]$Method,
    [Parameter(Mandatory = $true)][string]$Uri,
    [object]$Body = $null,
    [switch]$Raw
  )

  $token = Get-GitHubToken
  $headers = @{
    Authorization = "Bearer $token"
    Accept = "application/vnd.github+json"
    "X-GitHub-Api-Version" = "2022-11-28"
    "User-Agent" = "x1box-fork-workflow-bridge"
  }

  $invokeParams = @{
    Method = $Method
    Uri = $Uri
    Headers = $headers
  }

  if ($null -ne $Body) {
    $invokeParams["Body"] = ($Body | ConvertTo-Json -Depth 8)
    $invokeParams["ContentType"] = "application/json"
  }

  if ($Raw) {
    return Invoke-WebRequest @invokeParams
  }

  return Invoke-RestMethod @invokeParams
}

function Download-GitHubFile {
  param(
    [Parameter(Mandatory = $true)][string]$Uri,
    [Parameter(Mandatory = $true)][string]$OutFile
  )

  $token = Get-GitHubToken
  $headers = @{
    Authorization = "Bearer $token"
    Accept = "application/vnd.github+json"
    "X-GitHub-Api-Version" = "2022-11-28"
    "User-Agent" = "x1box-fork-workflow-bridge"
  }

  Invoke-WebRequest -Method GET -Uri $Uri -Headers $headers -OutFile $OutFile
}

function Start-WorkflowRun {
  param(
    [string]$Repository,
    [string]$WorkflowFile,
    [string]$BranchRef,
    [string]$SimulatorDestination,
    [bool]$ShouldRunDeviceBuild,
    [string]$ArtifactLabel
  )

  $repoParts = Get-RepositoryParts -Repository $Repository
  $dispatchUri = "https://api.github.com/repos/$($repoParts.Owner)/$($repoParts.Name)/actions/workflows/$WorkflowFile/dispatches"

  $body = @{
    ref = $BranchRef
    inputs = @{
      sim_destination = $SimulatorDestination
      run_device_build = if ($ShouldRunDeviceBuild) { "true" } else { "false" }
      artifact_name = $ArtifactLabel
    }
  }

  Invoke-GitHubApi -Method POST -Uri $dispatchUri -Body $body | Out-Null
  Write-Host "Workflow dispatched for $Repository on ref '$BranchRef'."
}

function Get-LatestWorkflowRun {
  param(
    [string]$Repository,
    [string]$WorkflowFile,
    [string]$BranchRef,
    [string]$ArtifactLabel
  )

  $repoParts = Get-RepositoryParts -Repository $Repository
  $runsUri = "https://api.github.com/repos/$($repoParts.Owner)/$($repoParts.Name)/actions/workflows/$WorkflowFile/runs?branch=$([uri]::EscapeDataString($BranchRef))&per_page=20"
  $response = Invoke-GitHubApi -Method GET -Uri $runsUri

  $candidate = $response.workflow_runs |
    Where-Object {
      $_.name -eq "Build iOS App" -and
      ($_.event -eq "workflow_dispatch" -or $_.event -eq "push" -or $_.event -eq "pull_request")
    } |
    Sort-Object created_at -Descending |
    Select-Object -First 1

  if (-not $candidate) {
    throw "No workflow run was found yet for $Repository / $WorkflowFile on ref '$BranchRef'."
  }

  return $candidate
}

function Get-WorkflowRun {
  param(
    [string]$Repository,
    [int64]$WorkflowRunId
  )

  $repoParts = Get-RepositoryParts -Repository $Repository
  $runUri = "https://api.github.com/repos/$($repoParts.Owner)/$($repoParts.Name)/actions/runs/$WorkflowRunId"
  return Invoke-GitHubApi -Method GET -Uri $runUri
}

function New-RunSummary {
  param(
    [object]$Run,
    [string]$Repository,
    [string]$WorkflowFile,
    [string]$ArtifactLabel
  )

  return [ordered]@{
    checked_at_utc = (Get-Date).ToUniversalTime().ToString("o")
    repository = $Repository
    workflow = $WorkflowFile
    artifact_name = $ArtifactLabel
    run_id = [int64]$Run.id
    run_number = [int]$Run.run_number
    status = [string]$Run.status
    conclusion = if ([string]::IsNullOrEmpty([string]$Run.conclusion)) { "pending" } else { [string]$Run.conclusion }
    event = [string]$Run.event
    name = [string]$Run.name
    head_branch = [string]$Run.head_branch
    head_sha = [string]$Run.head_sha
    html_url = [string]$Run.html_url
    created_at = [string]$Run.created_at
    updated_at = [string]$Run.updated_at
  }
}

function Write-RunSummary {
  param(
    [hashtable]$Summary,
    [string]$DestinationRoot,
    [string]$FileName
  )

  New-Item -ItemType Directory -Force -Path $DestinationRoot | Out-Null

  $latestPath = Join-Path $DestinationRoot $FileName
  $runPath = Join-Path $DestinationRoot ("run-" + $Summary.run_id + "-status.json")
  $json = $Summary | ConvertTo-Json -Depth 8

  Set-Content -Path $latestPath -Value $json -Encoding UTF8
  Set-Content -Path $runPath -Value $json -Encoding UTF8

  return $latestPath
}

function Get-LastSyncedRunId {
  param([string]$DestinationRoot)

  $marker = Join-Path $DestinationRoot ".last-synced-run.txt"
  if (-not (Test-Path $marker)) {
    return $null
  }

  $raw = (Get-Content $marker -ErrorAction SilentlyContinue | Select-Object -First 1)
  if ([string]::IsNullOrWhiteSpace($raw)) {
    return $null
  }

  return [int64]$raw
}

function Set-LastSyncedRunId {
  param(
    [string]$DestinationRoot,
    [int64]$WorkflowRunId
  )

  $marker = Join-Path $DestinationRoot ".last-synced-run.txt"
  Set-Content -Path $marker -Value ([string]$WorkflowRunId) -Encoding ASCII
}

function Wait-WorkflowRun {
  param(
    [string]$Repository,
    [int64]$WorkflowRunId,
    [int]$SleepSeconds,
    [int]$MaxMinutes
  )

  $deadline = (Get-Date).AddMinutes($MaxMinutes)

  do {
    $run = Get-WorkflowRun -Repository $Repository -WorkflowRunId $WorkflowRunId
    $conclusion = if ([string]::IsNullOrEmpty([string]$run.conclusion)) {
      "pending"
    } else {
      [string]$run.conclusion
    }
    $summary = "[{0}] status={1} conclusion={2}" -f $run.run_number, $run.status, $conclusion
    Write-Host $summary

    if ($run.status -eq "completed") {
      return $run
    }

    Start-Sleep -Seconds $SleepSeconds
  } while ((Get-Date) -lt $deadline)

  throw "Timed out while waiting for run $WorkflowRunId after $MaxMinutes minutes."
}

function Save-WorkflowArtifacts {
  param(
    [string]$Repository,
    [int64]$WorkflowRunId,
    [string]$DestinationRoot,
    [string]$ExpectedArtifactName
  )

  $repoParts = Get-RepositoryParts -Repository $Repository
  $artifactsUri = "https://api.github.com/repos/$($repoParts.Owner)/$($repoParts.Name)/actions/runs/$WorkflowRunId/artifacts"
  $artifactResponse = Invoke-GitHubApi -Method GET -Uri $artifactsUri

  if (-not $artifactResponse.artifacts) {
    throw "No artifacts were uploaded for run $WorkflowRunId."
  }

  $destination = Join-Path $DestinationRoot ("run-" + $WorkflowRunId)
  New-Item -ItemType Directory -Force -Path $destination | Out-Null

  foreach ($artifact in $artifactResponse.artifacts) {
    $matchesExpectedName = -not $ExpectedArtifactName -or $artifact.name -eq $ExpectedArtifactName
    $matchesFollowUpArtifact = $artifact.name -like "x1box-ios-followup-*"

    if (-not ($matchesExpectedName -or $matchesFollowUpArtifact)) {
      continue
    }

    $zipPath = Join-Path $destination ($artifact.name + ".zip")
    $extractPath = Join-Path $destination $artifact.name
    Download-GitHubFile -Uri $artifact.archive_download_url -OutFile $zipPath
    if (Test-Path $extractPath) {
      Remove-Item -Recurse -Force $extractPath
    }
    Expand-Archive -Path $zipPath -DestinationPath $extractPath -Force
    Write-Host "Downloaded artifact '$($artifact.name)' to $extractPath"
  }

  return $destination
}

$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$resolvedOutput = if ([System.IO.Path]::IsPathRooted($OutputDirectory)) {
  $OutputDirectory
} else {
  Join-Path $root $OutputDirectory
}

New-Item -ItemType Directory -Force -Path $resolvedOutput | Out-Null

if ($Mode -eq "dispatch" -or $Mode -eq "full") {
  Start-WorkflowRun `
    -Repository $Repo `
    -WorkflowFile $Workflow `
    -BranchRef $Ref `
    -SimulatorDestination $SimDestination `
    -ShouldRunDeviceBuild $RunDeviceBuild `
    -ArtifactLabel $ArtifactName

  if (-not $RunId) {
    Start-Sleep -Seconds 8
    $latestRun = Get-LatestWorkflowRun -Repository $Repo -WorkflowFile $Workflow -BranchRef $Ref -ArtifactLabel $ArtifactName
    $RunId = [int64]$latestRun.id
    Write-Host "Latest run id: $RunId"
  }
}

if (($Mode -eq "watch" -or $Mode -eq "download" -or $Mode -eq "full" -or $Mode -eq "status" -or $Mode -eq "sync") -and -not $RunId) {
  $latestRun = Get-LatestWorkflowRun -Repository $Repo -WorkflowFile $Workflow -BranchRef $Ref -ArtifactLabel $ArtifactName
  $RunId = [int64]$latestRun.id
  Write-Host "Resolved latest run id: $RunId"
}

if ($Mode -eq "status" -or $Mode -eq "sync") {
  $currentRun = Get-WorkflowRun -Repository $Repo -WorkflowRunId $RunId
  $summary = New-RunSummary -Run $currentRun -Repository $Repo -WorkflowFile $Workflow -ArtifactLabel $ArtifactName
  $summaryPath = Write-RunSummary -Summary $summary -DestinationRoot $resolvedOutput -FileName $StatusFileName

  Write-Host ("Latest workflow status: run #{0} status={1} conclusion={2}" -f $summary.run_number, $summary.status, $summary.conclusion)
  Write-Host ("Status summary saved to: " + $summaryPath)
  Write-Host ("Run URL: " + $summary.html_url)

  if ($Mode -eq "sync" -and $summary.status -eq "completed" -and $summary.conclusion -eq "success") {
    $lastSyncedRunId = Get-LastSyncedRunId -DestinationRoot $resolvedOutput
    if ($lastSyncedRunId -ne $summary.run_id) {
      $artifactPath = Save-WorkflowArtifacts -Repository $Repo -WorkflowRunId $summary.run_id -DestinationRoot $resolvedOutput -ExpectedArtifactName $ArtifactName
      Set-LastSyncedRunId -DestinationRoot $resolvedOutput -WorkflowRunId $summary.run_id
      Write-Host ("Artifacts saved under: " + $artifactPath)
    } else {
      Write-Host ("Artifacts for run {0} were already synced." -f $summary.run_id)
    }
  }
}

if ($Mode -eq "watch" -or $Mode -eq "full") {
  $completedRun = Wait-WorkflowRun -Repository $Repo -WorkflowRunId $RunId -SleepSeconds $PollSeconds -MaxMinutes $TimeoutMinutes
  Write-Host ("Run URL: " + $completedRun.html_url)
  if ($completedRun.conclusion -ne "success") {
    throw "Workflow run $RunId finished with conclusion '$($completedRun.conclusion)'."
  }
}

if ($Mode -eq "download" -or $Mode -eq "full") {
  $artifactPath = Save-WorkflowArtifacts -Repository $Repo -WorkflowRunId $RunId -DestinationRoot $resolvedOutput -ExpectedArtifactName $ArtifactName
  Write-Host ("Artifacts saved under: " + $artifactPath)
}
