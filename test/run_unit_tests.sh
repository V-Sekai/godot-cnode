#!/bin/bash
# Run unit tests for Godot CNode

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

cd "$PROJECT_ROOT"

# Build unit tests
echo "Building unit tests..."
scons platform=macos target=template_release arch=arm64 2>&1 | grep -E "(test_cnode_unit|Building|Error|error)" || true

# Check if test executable exists
if [ -f "bin/test_cnode_unit" ]; then
    echo ""
    echo "Running unit tests..."
    echo ""
    ./bin/test_cnode_unit
    exit_code=$?

    if [ $exit_code -eq 0 ]; then
        echo ""
        echo "✓ All unit tests passed!"
        exit 0
    else
        echo ""
        echo "✗ Some unit tests failed!"
        exit 1
    fi
else
    echo "Error: Test executable not found: bin/test_cnode_unit"
    echo "Make sure the build completed successfully."
    exit 1
fi
