"""Zero-dependency standalone test runner for Emebalachat.

Discovers and executes all unit tests using Python's standard library `unittest`
framework, printing formatted summaries and returning exit code 0 on complete success.
"""

from __future__ import annotations

import os
import sys
import time
import unittest
from pathlib import Path

# Ensure project root is present in sys.path
PROJECT_ROOT = Path(__file__).resolve().parent.parent
if str(PROJECT_ROOT) not in sys.path:
    sys.path.insert(0, str(PROJECT_ROOT))


def run_all_tests() -> int:
    """Discover and execute all tests in the tests/ directory.

    Returns:
        0 if all tests pass, 1 if any test fails or encounters an error.
    """
    tests_dir = Path(__file__).resolve().parent

    print("=" * 72)
    print("  Emebalachat Automated Unit Test Runner")
    print(f"  Project Root : {PROJECT_ROOT}")
    print(f"  Tests Dir    : {tests_dir}")
    print(f"  Python       : {sys.version.split()[0]} ({sys.platform})")
    print("=" * 72)

    # Initialize unittest test loader and test suite
    loader = unittest.TestLoader()
    suite = loader.discover(
        start_dir=str(tests_dir),
        pattern="test_*.py",
        top_level_dir=str(PROJECT_ROOT),
    )

    total_discovered = suite.countTestCases()
    print(f"Discovered {total_discovered} test cases across test modules.\n")

    # Execute tests with timing
    start_time = time.perf_counter()
    runner = unittest.TextTestRunner(verbosity=2)
    result = runner.run(suite)
    duration = time.perf_counter() - start_time

    # Calculate statistics
    total_run = result.testsRun
    failed = len(result.failures)
    errors = len(result.errors)
    skipped = len(result.skipped)
    passed = total_run - failed - errors - skipped

    print("\n" + "=" * 72)
    print("  Test Execution Summary")
    print("=" * 72)
    print(f"  Total Run : {total_run}")
    print(f"  Passed    : {passed}")
    print(f"  Failed    : {failed}")
    print(f"  Errors    : {errors}")
    print(f"  Skipped   : {skipped}")
    print(f"  Duration  : {duration:.3f}s")
    print("-" * 72)

    if result.wasSuccessful():
        print("  RESULT: ALL TESTS PASSED (SUCCESS)")
        print("=" * 72)
        return 0
    else:
        print("  RESULT: TEST SUITE FAILED")
        print("=" * 72)
        return 1


if __name__ == "__main__":
    exit_code = run_all_tests()
    sys.exit(exit_code)
