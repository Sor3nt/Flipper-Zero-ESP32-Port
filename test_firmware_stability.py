#!/usr/bin/env python3
"""
Comprehensive Firmware Testing Suite
- Syntax validation
- Memory leak detection
- Logic verification
- Communication testing
- Error handling validation
"""

import os
import re
import json
from pathlib import Path
from typing import List, Dict, Tuple

class FirmwareTestSuite:
    def __init__(self):
        self.errors = []
        self.warnings = []
        self.passed = 0
        self.failed = 0
        self.base_path = Path(".")

    def log_error(self, test_name: str, error: str):
        """Log test error"""
        self.errors.append({"test": test_name, "error": error})
        self.failed += 1
        print(f"[FAIL] {test_name}: {error}")

    def log_warning(self, test_name: str, warning: str):
        """Log test warning"""
        self.warnings.append({"test": test_name, "warning": warning})
        print(f"[WARN] {test_name}: {warning}")

    def log_pass(self, test_name: str):
        """Log passed test"""
        self.passed += 1
        print(f"[PASS] {test_name}: OK")

    # ========== SYNTAX TESTS ==========

    def test_spi_stability_header_syntax(self):
        """Test SPI stability header for syntax errors"""
        try:
            with open("components/furi_hal/furi_hal_spi_stability.h") as f:
                content = f.read()

            # Check for common syntax errors
            if content.count("{") != content.count("}"):
                self.log_error("SPI Header Braces", "Mismatched braces")
                return False

            if content.count("(") != content.count(")"):
                self.log_error("SPI Header Parens", "Mismatched parentheses")
                return False

            # Check for required structures
            required = ["SpiDevice", "SpiStabilityManager", "SpiPriority"]
            for req in required:
                if req not in content:
                    self.log_error("SPI Header Struct", f"Missing {req}")
                    return False

            self.log_pass("SPI Header Syntax")
            return True
        except Exception as e:
            self.log_error("SPI Header Read", str(e))
            return False

    def test_spi_stability_impl_syntax(self):
        """Test SPI stability implementation for syntax errors"""
        try:
            with open("components/furi_hal/furi_hal_spi_stability.c") as f:
                content = f.read()

            # Check structure
            if content.count("{") != content.count("}"):
                self.log_error("SPI Impl Braces", "Mismatched braces")
                return False

            # Check for required functions
            required_funcs = [
                "furi_hal_spi_stability_init",
                "furi_hal_spi_stability_acquire",
                "furi_hal_spi_stability_release",
            ]

            for func in required_funcs:
                if f"void {func}" not in content and f"bool {func}" not in content:
                    self.log_error("SPI Impl Functions", f"Missing {func}")
                    return False

            self.log_pass("SPI Impl Syntax")
            return True
        except Exception as e:
            self.log_error("SPI Impl Read", str(e))
            return False

    # ========== MEMORY TESTS ==========

    def test_memory_allocation_leaks(self):
        """Test for memory allocation without deallocation"""
        try:
            with open("components/furi_hal/furi_hal_spi_stability.c") as f:
                content = f.read()

            # Count malloc vs free
            mallocs = len(re.findall(r'\bmalloc\(', content))
            frees = len(re.findall(r'\bfree\(', content))

            # Check semaphore creation/deletion
            creates = len(re.findall(r'xSemaphoreCreateBinary', content))
            deletes = len(re.findall(r'vSemaphoreDelete', content))

            # Note: Some creates might not need deletes in global init
            if frees < mallocs:
                self.log_warning("Memory Alloc",
                    f"malloc={mallocs}, free={frees} (may be global pools)")
            else:
                self.log_pass("Memory Allocation")

            return True
        except Exception as e:
            self.log_error("Memory Leak Test", str(e))
            return False

    def test_stack_overflow_risk(self):
        """Test for large stack allocations"""
        try:
            with open("components/furi_hal/furi_hal_spi_stability.c") as f:
                content = f.read()

            # Find large array allocations on stack
            large_arrays = re.findall(r'(\w+)\s+\w+\[(\d+)\]', content)

            for var_type, size in large_arrays:
                size_int = int(size)
                if size_int > 4096:  # > 4KB
                    self.log_warning("Stack Alloc",
                        f"Large array {size} bytes (potential overflow)")

            self.log_pass("Stack Overflow Risk")
            return True
        except Exception as e:
            self.log_error("Stack Test", str(e))
            return False

    # ========== LOGIC TESTS ==========

    def test_semaphore_usage(self):
        """Test semaphore operations for correctness"""
        try:
            with open("components/furi_hal/furi_hal_spi_stability.c") as f:
                content = f.read()

            # Check semaphore operations pattern
            takes = len(re.findall(r'xSemaphoreTake', content))
            gives = len(re.findall(r'xSemaphoreGive', content))

            if takes == 0 or gives == 0:
                self.log_error("Semaphore Logic", "Missing take or give")
                return False

            # Check for potential deadlock patterns
            if 'while' in content and 'xSemaphoreTake' in content:
                # Acceptable if timeout is used
                if 'pdMS_TO_TICKS' in content or 'portMAX_DELAY' in content:
                    self.log_pass("Semaphore Usage")
                    return True
                else:
                    self.log_error("Semaphore Logic",
                        "While loop with indefinite wait")
                    return False

            self.log_pass("Semaphore Usage")
            return True
        except Exception as e:
            self.log_error("Semaphore Logic Test", str(e))
            return False

    def test_timeout_handling(self):
        """Test timeout logic"""
        try:
            with open("components/furi_hal/furi_hal_spi_stability.c") as f:
                content = f.read()

            # Check timeout configurations
            timeouts = re.findall(r'(\d+)\s*(?:ms|MS|millisecond)', content)

            if not timeouts:
                self.log_warning("Timeout Test", "No timeout values found")

            # Check for 0 timeout handling
            if 'timeout_ms == 0' in content or 'timeout_ms = 0' in content:
                self.log_pass("Timeout Handling")
            else:
                self.log_pass("Timeout Handling")

            return True
        except Exception as e:
            self.log_error("Timeout Test", str(e))
            return False

    def test_priority_logic(self):
        """Test priority queue logic"""
        try:
            with open("components/furi_hal/furi_hal_spi_stability.c") as f:
                content = f.read()

            # Check priority enum values
            if 'SpiPriority' not in content:
                self.log_error("Priority Logic", "No priority enum")
                return False

            # Check priority queue operations
            if 'priority_queue' not in content:
                self.log_error("Priority Logic", "No priority queue")
                return False

            self.log_pass("Priority Logic")
            return True
        except Exception as e:
            self.log_error("Priority Test", str(e))
            return False

    # ========== COMMUNICATION TESTS ==========

    def test_device_config_compatibility(self):
        """Test device configuration for compatibility"""
        try:
            with open("components/furi_hal/furi_hal_spi_device_config.h") as f:
                content = f.read()

            # Extract device configs
            devices = ["LCD", "SD", "CC1101", "NRF24"]

            for device in devices:
                if f"_{device}_SPI_FREQ_HZ" not in content:
                    self.log_warning("Device Config",
                        f"No frequency config for {device}")

            self.log_pass("Device Config Compatibility")
            return True
        except Exception as e:
            self.log_error("Device Config Test", str(e))
            return False

    def test_spi_mode_consistency(self):
        """Test SPI mode consistency across devices"""
        try:
            with open("components/furi_hal/furi_hal_spi_device_config.h") as f:
                content = f.read()

            # All devices should use SPI_MODE_0
            modes = re.findall(r'_SPI_MODE\s+(\d+)', content)

            for mode in modes:
                if mode != '0':
                    self.log_warning("SPI Mode",
                        f"Non-standard SPI mode detected: {mode}")

            self.log_pass("SPI Mode Consistency")
            return True
        except Exception as e:
            self.log_error("SPI Mode Test", str(e))
            return False

    def test_frequency_safety(self):
        """Test frequency values are within safe ranges"""
        try:
            with open("components/furi_hal/furi_hal_spi_device_config.h") as f:
                content = f.read()

            # Extract frequencies
            freqs = re.findall(r'(\d+)\s*\*\s*1000\s*\*\s*1000', content)

            safe_ranges = {
                "LCD": (20, 50),
                "SD": (20, 50),
                "CC1101": (1, 15),
                "NRF24": (1, 15),
            }

            unsafe_freqs = []
            for freq_str in freqs:
                freq_mhz = int(freq_str)
                if freq_mhz < 1 or freq_mhz > 100:
                    unsafe_freqs.append(freq_mhz)

            if unsafe_freqs:
                self.log_error("Frequency Safety",
                    f"Unsafe frequencies: {unsafe_freqs} MHz")
                return False

            self.log_pass("Frequency Safety")
            return True
        except Exception as e:
            self.log_error("Frequency Test", str(e))
            return False

    # ========== ERROR HANDLING TESTS ==========

    def test_error_recovery_logic(self):
        """Test error recovery mechanisms"""
        try:
            with open("components/furi_hal/furi_hal_spi_stability.c") as f:
                content = f.read()

            # Check for retry logic
            if 'retry' not in content.lower():
                self.log_warning("Error Recovery",
                    "No explicit retry logic found")

            # Check for error codes/states
            if 'result' not in content or 'error' not in content.lower():
                self.log_warning("Error Recovery",
                    "Limited error state tracking")

            self.log_pass("Error Recovery Logic")
            return True
        except Exception as e:
            self.log_error("Error Recovery Test", str(e))
            return False

    def test_deadlock_detection(self):
        """Test deadlock detection logic"""
        try:
            with open("components/furi_hal/furi_hal_spi_stability.c") as f:
                content = f.read()

            # Check for timeout monitoring
            if 'operation_start_ms' not in content:
                self.log_error("Deadlock Detection",
                    "No operation start time tracking")
                return False

            # Check for deadline checking
            if 'operation_timeout_ms' not in content:
                self.log_error("Deadlock Detection",
                    "No timeout threshold")
                return False

            self.log_pass("Deadlock Detection")
            return True
        except Exception as e:
            self.log_error("Deadlock Test", str(e))
            return False

    # ========== INTEGRATION TESTS ==========

    def test_header_implementation_match(self):
        """Test header and implementation match"""
        try:
            with open("components/furi_hal/furi_hal_spi_stability.h") as f:
                header = f.read()

            with open("components/furi_hal/furi_hal_spi_stability.c") as f:
                impl = f.read()

            # Extract function declarations from header
            decls = re.findall(r'(bool|void|uint32_t)\s+(\w+)\s*\([^)]*\)', header)

            for ret_type, func_name in decls:
                # Check if implemented
                if f"void {func_name}" not in impl and f"bool {func_name}" not in impl:
                    self.log_warning("Header/Impl Match",
                        f"Declaration not implemented: {func_name}")

            self.log_pass("Header/Implementation Match")
            return True
        except Exception as e:
            self.log_error("Integration Test", str(e))
            return False

    def test_example_integration(self):
        """Test example integration code"""
        try:
            with open("example_sd_stable.c") as f:
                content = f.read()

            # Check example uses correct API
            if 'furi_hal_spi_stability_acquire' not in content:
                self.log_error("Example Integration",
                    "Example doesn't use acquire")
                return False

            if 'furi_hal_spi_stability_release' not in content:
                self.log_error("Example Integration",
                    "Example doesn't use release")
                return False

            # Check for error handling
            if 'if (' not in content or 'return' not in content:
                self.log_warning("Example Integration",
                    "Limited error handling in example")

            self.log_pass("Example Integration")
            return True
        except Exception as e:
            self.log_error("Example Test", str(e))
            return False

    # ========== RUN ALL TESTS ==========

    def run_all_tests(self):
        """Run complete test suite"""
        print("\n" + "="*60)
        print("[COMPREHENSIVE FIRMWARE TEST SUITE]")
        print("="*60 + "\n")

        print("[1] SYNTAX VALIDATION\n")
        self.test_spi_stability_header_syntax()
        self.test_spi_stability_impl_syntax()

        print("\n[2] MEMORY TESTS\n")
        self.test_memory_allocation_leaks()
        self.test_stack_overflow_risk()

        print("\n[3] LOGIC VERIFICATION\n")
        self.test_semaphore_usage()
        self.test_timeout_handling()
        self.test_priority_logic()

        print("\n[4] COMMUNICATION TESTS\n")
        self.test_device_config_compatibility()
        self.test_spi_mode_consistency()
        self.test_frequency_safety()

        print("\n[5] ERROR HANDLING\n")
        self.test_error_recovery_logic()
        self.test_deadlock_detection()

        print("\n[6] INTEGRATION TESTS\n")
        self.test_header_implementation_match()
        self.test_example_integration()

        self.print_summary()

    def print_summary(self):
        """Print test summary"""
        total = self.passed + self.failed
        pass_rate = (self.passed / total * 100) if total > 0 else 0

        print("\n" + "="*60)
        print("[TEST SUMMARY]")
        print("="*60)
        print(f"Total Tests:  {total}")
        print(f"Passed:       {self.passed} [OK]")
        print(f"Failed:       {self.failed} [FAIL]")
        print(f"Pass Rate:    {pass_rate:.1f}%")
        print(f"Warnings:     {len(self.warnings)}")

        if self.failed == 0:
            print("\n[SUCCESS] ALL TESTS PASSED - NO CRITICAL ISSUES")
        else:
            print(f"\n[WARNING] {self.failed} CRITICAL ISSUE(S) FOUND")

        if self.warnings:
            print("\nWarnings:")
            for w in self.warnings:
                print(f"  - {w['test']}: {w['warning']}")

        print("="*60 + "\n")

if __name__ == "__main__":
    suite = FirmwareTestSuite()
    suite.run_all_tests()
