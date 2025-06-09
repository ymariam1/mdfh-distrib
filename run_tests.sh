#!/bin/bash

# Wrapper script for running MDFH tests
# Calls the actual test script in the scripts directory

echo "Running MDFH tests..."
exec ./scripts/run_tests.sh "$@" 