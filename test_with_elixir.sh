#!/bin/bash

echo "=== Godot CNode Elixir Test ==="
echo ""

# Check if epmd is running
echo "Checking epmd status..."
if epmd -names > /dev/null 2>&1; then
    echo "✓ epmd is running"
    if epmd -names | grep -q "godot"; then
        echo "  ✓ CNode is registered"
    else
        echo "  ⚠ CNode not registered (might need to restart Godot)"
    fi
else
    echo "⚠ epmd is not running"
    echo "Starting epmd..."
    epmd -daemon
    sleep 1
    echo "✓ epmd started"
    echo ""
    echo "⚠ IMPORTANT: Restart Godot now so the CNode can register with epmd"
    echo "   The CNode needs epmd running at startup to use ei_publish()"
    echo ""
    read -p "Press Enter after restarting Godot, or Ctrl+C to cancel..."
fi

echo ""
echo "Checking if CNode is registered with epmd..."
if epmd -names | grep -q "godot"; then
    echo "  ✓ CNode is registered with epmd"
else
    echo "  ⚠ CNode not registered with epmd"
    echo "  This usually means:"
    echo "    1. Godot is not running, OR"
    echo "    2. Godot is running but CNode GDExtension is not loaded, OR"
    echo "    3. Godot was started before epmd was running (CNode used ei_listen fallback)"
    echo "  Solution:"
    echo "    - Make sure Godot is running with the CNode GDExtension loaded"
    echo "    - If Godot was already running, restart it now (with epmd running)"
    echo ""
    read -p "Press Enter to continue anyway (test will likely fail), or Ctrl+C to cancel..."
fi

echo ""
echo "Running Elixir test..."
echo ""

# Check if Godot is running
if ! pgrep -x "Godot" > /dev/null; then
    echo "Error: Godot is not running. Please start Godot with the CNode GDExtension loaded."
    exit 1
fi
echo "✓ Godot is running."

# Run the test (it will auto-detect hostname)
elixir test/cnode_test_connect.exs
