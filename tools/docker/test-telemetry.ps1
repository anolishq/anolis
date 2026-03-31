#!/usr/bin/env pwsh
# Anolis Telemetry Stack Test Script (Windows PowerShell)
# Tests full observability pipeline: Runtime -> InfluxDB -> Grafana

param(
    [switch]$SkipBuild,
    [switch]$KeepRunning
)

$ErrorActionPreference = "Stop"

# Colors for output
function Write-Step { Write-Host "> $args" -ForegroundColor Cyan }
function Write-Success { Write-Host "[OK] $args" -ForegroundColor Green }
function Write-Warning { Write-Host "[WARN] $args" -ForegroundColor Yellow }
function Write-Error { Write-Host "[FAIL] $args" -ForegroundColor Red }

Write-Host ""
Write-Host "==========================================" -ForegroundColor Cyan
Write-Host "  Anolis Telemetry Test" -ForegroundColor Cyan
Write-Host "==========================================" -ForegroundColor Cyan
Write-Host ""

# Check prerequisites
Write-Step "Checking prerequisites..."

if (-not (Get-Command docker -ErrorAction SilentlyContinue)) {
    Write-Error "Docker not found. Install Docker Desktop first."
    exit 1
}
Write-Success "Docker installed"

if (-not (Get-Command docker-compose -ErrorAction SilentlyContinue)) {
    if (-not (Get-Command "docker compose" -ErrorAction SilentlyContinue)) {
        Write-Error "Docker Compose not found"
        exit 1
    }
}
Write-Success "Docker Compose available"

# Check if runtime is built
$runtimePath = "..\..\build\dev-windows-release\core\Release\anolis-runtime.exe"
if (-not (Test-Path $runtimePath)) {
    Write-Warning "Runtime not built at $runtimePath"
    if (-not $SkipBuild) {
        Write-Step "Building runtime..."
        Push-Location ..\..
        cmake --preset dev-windows-release
        cmake --build --preset dev-windows-release --parallel
        Pop-Location
    } else {
        Write-Error "Runtime not found and -SkipBuild specified"
        exit 1
    }
}
Write-Success "Runtime executable found"

# Check provider-sim
$providerPath = "..\..\..\anolis-provider-sim\build\dev-windows-release\Release\anolis-provider-sim.exe"
if (-not (Test-Path $providerPath)) {
    Write-Error "Provider-sim not found at $providerPath"
    Write-Host "Run: cd ..\anolis-provider-sim; cmake --preset dev-windows-release; cmake --build --preset dev-windows-release --parallel"
    exit 1
}
Write-Success "Provider-sim found"

Write-Host ""
Write-Step "Step 1: Starting Docker stack (InfluxDB + Grafana)..."
Write-Host ""

docker compose -f docker-compose.observability.yml down -v 2>$null
Start-Sleep -Seconds 2

docker compose -f docker-compose.observability.yml up -d

Write-Host ""
Write-Step "Waiting for containers to be healthy (30s)..."
$waited = 0
while ($waited -lt 30) {
    $health = docker compose -f docker-compose.observability.yml ps --format json | ConvertFrom-Json | Where-Object { $_.Service -eq "influxdb" }
    if ($health.Health -eq "healthy") {
        Write-Success "InfluxDB is healthy"
        break
    }
    Start-Sleep -Seconds 2
    $waited += 2
    Write-Host "." -NoNewline
}
Write-Host ""

if ($waited -ge 30) {
    Write-Warning "InfluxDB health check timeout, but continuing..."
}

Write-Host ""
Write-Step "Step 2: Verifying InfluxDB is accessible..."
Start-Sleep -Seconds 3

try {
    $response = Invoke-WebRequest -Uri "http://localhost:8086/health" -Method Get -TimeoutSec 5
    if ($response.StatusCode -eq 200) {
        Write-Success "InfluxDB responding at http://localhost:8086"
    }
} catch {
    Write-Warning "InfluxDB health check failed: $($_.Exception.Message)"
    Write-Host "Continuing anyway..."
}

Write-Host ""
Write-Step "Step 3: Verifying Grafana is accessible..."
try {
    $response = Invoke-WebRequest -Uri "http://localhost:3000/api/health" -Method Get -TimeoutSec 5
    if ($response.StatusCode -eq 200) {
        Write-Success "Grafana responding at http://localhost:3000"
    }
} catch {
    Write-Warning "Grafana health check failed, but it might still be starting..."
}

Write-Host ""
Write-Step "Step 4: Starting Anolis runtime with telemetry..."
Write-Host ""

# Resolve telemetry config path from repo root
$runtimeConfigPath = Resolve-Path "..\..\anolis-runtime-telemetry.yaml"

# Start runtime in background
$runtimeJob = Start-Job -ScriptBlock {
    param($runtimePath, $runtimeConfigPath)
    Set-Location (Split-Path $runtimePath)
    & $runtimePath "--config=$runtimeConfigPath"
} -ArgumentList (Resolve-Path $runtimePath), $runtimeConfigPath

Write-Success "Runtime job started (Job ID: $($runtimeJob.Id))"
Write-Host ""
Write-Host "Watching runtime output (first 15 seconds)..." -ForegroundColor Yellow
Write-Host ""

# Watch for key startup messages
$startTime = Get-Date
while (((Get-Date) - $startTime).TotalSeconds -lt 15) {
    $output = Receive-Job $runtimeJob -ErrorAction SilentlyContinue
    if ($output) {
        $output | ForEach-Object {
            if ($_ -match "Telemetry sink started") {
                Write-Host $_ -ForegroundColor Green
            } elseif ($_ -match "WARNING|ERROR") {
                Write-Host $_ -ForegroundColor Red
            } else {
                Write-Host $_
            }
        }
    }
    
    if ($runtimeJob.State -eq "Failed" -or $runtimeJob.State -eq "Stopped") {
        Write-Error "Runtime job failed!"
        Receive-Job $runtimeJob
        break
    }
    
    Start-Sleep -Milliseconds 500
}

Write-Host ""
Write-Step "Step 5: Checking data flow (waiting 10 seconds for events)..."
Start-Sleep -Seconds 10

Write-Host ""
Write-Host "=========================================="
Write-Host "  Validation Steps (Manual)"
Write-Host "=========================================="
Write-Host ""
Write-Host "1. InfluxDB Data Explorer:" -ForegroundColor Cyan
Write-Host "   http://localhost:8086" -ForegroundColor White
Write-Host "   Login: admin / anolis123" -ForegroundColor Gray
Write-Host "   -> Data Explorer -> Select bucket 'anolis'" -ForegroundColor Gray
Write-Host "   -> Query: anolis_signal" -ForegroundColor Gray
Write-Host "   -> Click Submit - you should see data points" -ForegroundColor Gray
Write-Host ""
Write-Host "2. Grafana Dashboards:" -ForegroundColor Cyan
Write-Host "   http://localhost:3000" -ForegroundColor White
Write-Host "   Login: admin / anolis123" -ForegroundColor Gray
Write-Host "   -> Dashboards -> Browse -> Anolis folder" -ForegroundColor Gray
Write-Host "   -> Open 'Signal History' - time-series should appear" -ForegroundColor Gray
Write-Host ""
Write-Host "3. Operator UI (Optional):" -ForegroundColor Cyan
Write-Host "   http://localhost:8080 (via runtime)" -ForegroundColor White
Write-Host "   -> Should show real-time updates via SSE" -ForegroundColor Gray
Write-Host ""
Write-Host "=========================================="
Write-Host ""

if ($KeepRunning) {
    Write-Host "Runtime will keep running. Press Ctrl+C to stop." -ForegroundColor Yellow
    Write-Host ""
    
    # Keep showing runtime output
    while ($runtimeJob.State -eq "Running") {
        $output = Receive-Job $runtimeJob -ErrorAction SilentlyContinue
        if ($output) {
            $output | ForEach-Object { Write-Host $_ }
        }
        Start-Sleep -Milliseconds 500
    }
} else {
    Write-Host "Test running for 30 more seconds, then cleaning up..." -ForegroundColor Yellow
    Write-Host "(Use -KeepRunning to keep services running)" -ForegroundColor Gray
    Write-Host ""
    
    # Show a few more runtime outputs
    for ($i = 0; $i -lt 30; $i++) {
        $output = Receive-Job $runtimeJob -ErrorAction SilentlyContinue
        if ($output) {
            $output | ForEach-Object { 
                if ($_ -match "InfluxSink.*Written") {
                    Write-Host $_ -ForegroundColor Green
                }
            }
        }
        Start-Sleep -Seconds 1
    }
    
    Write-Host ""
    Write-Step "Cleaning up..."
    
    # Stop runtime
    Stop-Job $runtimeJob -ErrorAction SilentlyContinue
    Remove-Job $runtimeJob -Force -ErrorAction SilentlyContinue
    Write-Success "Runtime stopped"
    
    # Stop Docker stack
    docker compose -f docker-compose.observability.yml down
    Write-Success "Docker stack stopped"
    
    Write-Host ""
    Write-Host "To keep services running for manual testing, use:" -ForegroundColor Yellow
    Write-Host "  .\test-telemetry.ps1 -KeepRunning" -ForegroundColor White
    Write-Host ""
}

Write-Host ""
Write-Success "Telemetry test complete!"
Write-Host ""
