#!/usr/bin/env bash
# Anolis Telemetry Stack Test Script (Linux/macOS)
# Tests full observability pipeline: Runtime -> InfluxDB -> Grafana

set -e

SKIP_BUILD=false
KEEP_RUNNING=false

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --skip-build)
            SKIP_BUILD=true
            shift
            ;;
        --keep-running)
            KEEP_RUNNING=true
            shift
            ;;
        *)
            echo "Unknown option: $1"
            echo "Usage: $0 [--skip-build] [--keep-running]"
            exit 1
            ;;
    esac
done

# Colors
CYAN='\033[0;36m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
GRAY='\033[0;37m'
NC='\033[0m' # No Color

echo ""
echo -e "${CYAN}==========================================${NC}"
echo -e "${CYAN}  Anolis Telemetry Test${NC}"
echo -e "${CYAN}==========================================${NC}"
echo ""

# Check prerequisites
echo -e "${CYAN}> Checking prerequisites...${NC}"

if ! command -v docker &> /dev/null; then
    echo -e "${RED}[FAIL] Docker not found. Install Docker first.${NC}"
    exit 1
fi
echo -e "${GREEN}[OK] Docker installed${NC}"

if ! command -v docker-compose &> /dev/null && ! docker compose version &> /dev/null; then
    echo -e "${RED}[FAIL] Docker Compose not found${NC}"
    exit 1
fi
echo -e "${GREEN}[OK] Docker Compose available${NC}"

# Check if runtime is built
RUNTIME_PATH="../../build/dev-release/core/anolis-runtime"
if [ ! -f "$RUNTIME_PATH" ]; then
    echo -e "${YELLOW}[WARN] Runtime not built at $RUNTIME_PATH${NC}"
    if [ "$SKIP_BUILD" = false ]; then
        echo -e "${CYAN}> Building runtime...${NC}"
        cd ../..
        cmake --preset dev-release
        cmake --build --preset dev-release --parallel
        cd tools/docker
    else
        echo -e "${RED}[FAIL] Runtime not found and --skip-build specified${NC}"
        exit 1
    fi
fi
echo -e "${GREEN}[OK] Runtime executable found${NC}"

# Check provider-sim
PROVIDER_PATH="../../../anolis-provider-sim/build/dev-release/anolis-provider-sim"
if [ ! -f "$PROVIDER_PATH" ]; then
    echo -e "${RED}[FAIL] Provider-sim not found at $PROVIDER_PATH${NC}"
    echo "Run: cd ../anolis-provider-sim && cmake --preset dev-release && cmake --build --preset dev-release --parallel"
    exit 1
fi
echo -e "${GREEN}[OK] Provider-sim found${NC}"

echo ""
echo -e "${CYAN}> Step 1: Starting Docker stack (InfluxDB + Grafana)...${NC}"
echo ""

docker compose -f docker-compose.observability.yml down -v 2>/dev/null || true
sleep 2

docker compose -f docker-compose.observability.yml up -d

echo ""
echo -e "${CYAN}> Waiting for containers to be healthy (30s)...${NC}"
for i in {1..30}; do
    if docker compose -f docker-compose.observability.yml ps | grep -q "healthy"; then
        echo -e "${GREEN}[OK] InfluxDB is healthy${NC}"
        break
    fi
    echo -n "."
    sleep 1
done
echo ""

sleep 3

echo ""
echo -e "${CYAN}> Step 2: Verifying InfluxDB is accessible...${NC}"
if curl -s -o /dev/null -w "%{http_code}" http://localhost:8086/health | grep -q "200"; then
    echo -e "${GREEN}[OK] InfluxDB responding at http://localhost:8086${NC}"
else
    echo -e "${YELLOW}[WARN] InfluxDB health check failed, continuing anyway...${NC}"
fi

echo ""
echo -e "${CYAN}> Step 3: Verifying Grafana is accessible...${NC}"
if curl -s -o /dev/null -w "%{http_code}" http://localhost:3000/api/health | grep -q "200"; then
    echo -e "${GREEN}[OK] Grafana responding at http://localhost:3000${NC}"
else
    echo -e "${YELLOW}[WARN] Grafana might still be starting...${NC}"
fi

echo ""
echo -e "${CYAN}> Step 4: Starting Anolis runtime with telemetry...${NC}"
echo ""

# Start runtime in background
cd ../..
./build/dev-release/core/anolis-runtime --config=./anolis-runtime-telemetry.yaml &
RUNTIME_PID=$!
cd tools/docker

echo -e "${GREEN}[OK] Runtime started (PID: $RUNTIME_PID)${NC}"
echo ""
echo -e "${YELLOW}Watching runtime output (first 15 seconds)...${NC}"
echo ""

sleep 15

echo ""
echo -e "${CYAN}> Step 5: Checking data flow (waiting 10 seconds for events)...${NC}"
sleep 10

echo ""
echo "=========================================="
echo -e "  ${CYAN}Validation Steps (Manual)${NC}"
echo "=========================================="
echo ""
echo -e "${CYAN}1. InfluxDB Data Explorer:${NC}"
echo "   http://localhost:8086"
echo -e "   ${GRAY}Login: admin / anolis123${NC}"
echo -e "   ${GRAY}-> Data Explorer -> Select bucket 'anolis'${NC}"
echo -e "   ${GRAY}-> Query: anolis_signal${NC}"
echo -e "   ${GRAY}-> Click Submit - you should see data points${NC}"
echo ""
echo -e "${CYAN}2. Grafana Dashboards:${NC}"
echo "   http://localhost:3000"
echo -e "   ${GRAY}Login: admin / anolis123${NC}"
echo -e "   ${GRAY}-> Dashboards -> Browse -> Anolis folder${NC}"
echo -e "   ${GRAY}-> Open 'Signal History' - time-series should appear${NC}"
echo ""
echo -e "${CYAN}3. Operator UI (Optional):${NC}"
echo "   http://localhost:8080 (via runtime)"
echo -e "   ${GRAY}-> Should show real-time updates via SSE${NC}"
echo ""
echo "=========================================="
echo ""

if [ "$KEEP_RUNNING" = true ]; then
    echo -e "${YELLOW}Runtime will keep running. Press Ctrl+C to stop.${NC}"
    echo ""
    
    # Keep running
    wait $RUNTIME_PID
else
    echo -e "${YELLOW}Test running for 30 more seconds, then cleaning up...${NC}"
    echo -e "${GRAY}(Use --keep-running to keep services running)${NC}"
    echo ""
    
    sleep 30
    
    echo ""
    echo -e "${CYAN}> Cleaning up...${NC}"
    
    # Stop runtime
    kill $RUNTIME_PID 2>/dev/null || true
    echo -e "${GREEN}[OK] Runtime stopped${NC}"
    
    # Stop Docker stack
    docker compose -f docker-compose.observability.yml down
    echo -e "${GREEN}[OK] Docker stack stopped${NC}"
    
    echo ""
    echo -e "${YELLOW}To keep services running for manual testing, use:${NC}"
    echo -e "  ${NC}./test-telemetry.sh --keep-running${NC}"
    echo ""
fi

echo ""
echo -e "${GREEN}[OK] Telemetry test complete!${NC}"
echo ""
