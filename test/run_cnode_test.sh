#!/bin/bash
# Run CNode integration tests with a real Godot instance

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
GODOT_BIN="${GODOT_BIN:-godot}"

echo "=== CNode Integration Test ==="
echo "Project root: $PROJECT_ROOT"
echo "Godot binary: $GODOT_BIN"

# Check if Godot is available
if ! command -v "$GODOT_BIN" &> /dev/null; then
    echo "Error: Godot binary not found: $GODOT_BIN"
    echo "Set GODOT_BIN environment variable or install Godot"
    exit 1
fi

# Check if Elixir is available
if ! command -v elixir &> /dev/null; then
    echo "Error: Elixir not found. Please install Elixir."
    exit 1
fi

# Check if Erlang is available
if ! command -v erl &> /dev/null; then
    echo "Error: Erlang not found. Please install Erlang."
    exit 1
fi

echo "✓ Godot found: $($GODOT_BIN --version)"
echo "✓ Elixir found: $(elixir --version | head -1)"
echo "✓ Erlang found: $(erl -version 2>&1 | head -1)"

# Start Godot in headless mode with the project
echo ""
echo "Starting Godot with CNode GDExtension..."
cd "$PROJECT_ROOT/bin"

# Start Godot in the background
"$GODOT_BIN" --headless --path . &
GODOT_PID=$!

# Wait a bit for Godot to start and CNode to initialize
echo "Waiting for Godot and CNode to start..."
sleep 5

# Check if Godot is still running
if ! kill -0 $GODOT_PID 2>/dev/null; then
    echo "Error: Godot process died"
    exit 1
fi

echo "✓ Godot started (PID: $GODOT_PID)"

# Run Elixir tests
echo ""
echo "Running Elixir CNode tests..."
cd "$PROJECT_ROOT"

# Run connectivity test first (simplest, most reliable)
if [ -f test/cnode_connectivity_test.exs ]; then
    echo "Running connectivity test..."
    elixir test/cnode_connectivity_test.exs
    TEST_RESULT=$?
    
    # If connectivity works, try full API test
    if [ $TEST_RESULT -eq 0 ] && [ -f test/cnode_test.exs ]; then
        echo ""
        echo "Running full API test..."
        elixir test/cnode_test.exs || TEST_RESULT=$?
    fi
else
    # Fallback to original test
    elixir test/cnode_test.exs
    TEST_RESULT=$?
fi

# Cleanup: kill Godot
echo ""
echo "Stopping Godot..."
kill $GODOT_PID 2>/dev/null || true
wait $GODOT_PID 2>/dev/null || true

if [ $TEST_RESULT -eq 0 ]; then
    echo "✓ All tests passed!"
    exit 0
else
    echo "✗ Tests failed"
    exit $TEST_RESULT
fi

