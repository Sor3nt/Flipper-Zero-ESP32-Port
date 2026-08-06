#!/usr/bin/env python3
"""
REAPER FURY FIRMWARE AUDIT TOOL
Analyzes boot logs, performance metrics, and detects optimization-related issues
"""

import re
import sys
from datetime import datetime
from typing import List, Dict, Tuple

class ReaperFuryAuditor:
    def __init__(self, log_file: str):
        self.log_file = log_file
        self.logs = []
        self.issues = []
        self.metrics = {}
        self.load_logs()
    
    def load_logs(self):
        """Load and parse device logs"""
        try:
            with open(self.log_file, 'r') as f:
                self.logs = f.readlines()
            print(f"✓ Loaded {len(self.logs)} log lines from {self.log_file}")
        except FileNotFoundError:
            print(f"✗ Log file not found: {self.log_file}")
            sys.exit(1)
    
    def check_boot_sequence(self):
        """Verify service startup sequence and timing"""
        print("\n[1] CHECKING BOOT SEQUENCE...")
        
        boot_start = None
        service_logs = []
        sd_mount_time = None
        
        for i, line in enumerate(self.logs):
            if 'app_main' in line and 'startup' in line:
                boot_start = i
                print(f"  Boot started at log line {i}")
            
            if 'started' in line.lower() and 'service' in line.lower():
                service_logs.append(line.strip())
            
            if 'sd_mounted' in line or 'Storage service ready' in line:
                sd_mount_time = line.strip()
                print(f"  SD Mount: {sd_mount_time}")
        
        # Check service startup order
        expected_order = ['storage', 'gui', 'desktop', 'dialogs']
        print(f"  Found {len(service_logs)} service startup messages")
        
        if not sd_mount_time:
            self.issues.append({
                'severity': 'HIGH',
                'issue': 'SD Mount Not Detected',
                'description': 'No SD mount completion message in logs',
                'recommendation': 'Check if SD card is present and working'
            })
        
        self.metrics['services_started'] = len(service_logs)
    
    def check_sd_mount_race_condition(self):
        """Detect potential SD mount race conditions"""
        print("\n[2] CHECKING SD MOUNT RACE CONDITIONS...")
        
        sd_mount_started = False
        sd_mount_complete = False
        file_access_before_mount = []
        
        for line in self.logs:
            if 'furi_hal_sd_mount' in line or 'sd_mount_card' in line:
                sd_mount_started = True
                print(f"  SD mount started")
            
            if 'sd_mounted=1' in line or 'SD card successfully' in line:
                sd_mount_complete = True
                print(f"  SD mount complete")
            
            # Check for file access before mount complete
            if sd_mount_started and not sd_mount_complete:
                if 'storage_sd_mount' in line or 'open' in line or '/ext/' in line:
                    file_access_before_mount.append(line.strip())
        
        if file_access_before_mount:
            self.issues.append({
                'severity': 'HIGH',
                'issue': 'Potential SD Mount Race Condition',
                'description': f'Detected {len(file_access_before_mount)} file operations before SD mount complete',
                'recommendation': 'Verify SD mount timing, add explicit wait before file ops'
            })
            print(f"  ⚠ Found {len(file_access_before_mount)} potential race conditions")
        else:
            print("  ✓ No SD mount race conditions detected")
    
    def check_display_rendering(self):
        """Verify display optimization metrics"""
        print("\n[3] CHECKING DISPLAY RENDERING...")
        
        stripe_height = None
        dma_timeout = None
        fps_cap = None
        
        for line in self.logs:
            if 'STRIPE_HEIGHT' in line:
                match = re.search(r'STRIPE_HEIGHT[:\s]*(\d+)', line)
                if match:
                    stripe_height = int(match.group(1))
            
            if 'LCD_DMA_TIMEOUT' in line or 'dma.*timeout' in line.lower():
                match = re.search(r'timeout[:\s]*(\d+)', line)
                if match:
                    dma_timeout = int(match.group(1))
            
            if 'GUI_FRAME_TIME' in line or 'frame.*60' in line.lower():
                fps_cap = 60
        
        print(f"  Stripe Height: {stripe_height or 'Not detected'}")
        print(f"  DMA Timeout: {dma_timeout or 'Not detected'} ms")
        print(f"  FPS Cap: {fps_cap or 'Not detected'} FPS")
        
        if stripe_height and stripe_height < 16:
            self.issues.append({
                'severity': 'MEDIUM',
                'issue': 'Display Stripe Height May Be Too Small',
                'description': f'Stripe height {stripe_height}px may cause rendering delays',
                'recommendation': 'Increase to 16px for optimal performance'
            })
        
        if dma_timeout and dma_timeout < 50:
            self.issues.append({
                'severity': 'MEDIUM',
                'issue': 'DMA Timeout Too Aggressive',
                'description': f'DMA timeout {dma_timeout}ms may cause display corruption',
                'recommendation': 'Increase to 50-100ms for safety margin'
            })
        
        self.metrics['stripe_height'] = stripe_height
        self.metrics['dma_timeout'] = dma_timeout
        self.metrics['fps_cap'] = fps_cap
    
    def check_nfc_performance(self):
        """Verify NFC polling optimization"""
        print("\n[4] CHECKING NFC PERFORMANCE...")
        
        nfc_polling_interval = None
        nfc_detections = []
        nfc_errors = []
        
        for line in self.logs:
            if 'ISO14443' in line or 'NFC' in line:
                if 'polling' in line.lower():
                    match = re.search(r'(\d+)\s*ms', line)
                    if match:
                        nfc_polling_interval = int(match.group(1))
                
                if 'detect' in line.lower() or 'found' in line.lower():
                    nfc_detections.append(line.strip())
                
                if 'error' in line.lower() or 'fail' in line.lower():
                    nfc_errors.append(line.strip())
        
        print(f"  NFC Polling Interval: {nfc_polling_interval or 'Not detected'} ms")
        print(f"  NFC Detections: {len(nfc_detections)}")
        print(f"  NFC Errors: {len(nfc_errors)}")
        
        if nfc_polling_interval and nfc_polling_interval < 50:
            self.issues.append({
                'severity': 'MEDIUM',
                'issue': 'NFC Polling Too Aggressive',
                'description': f'Polling interval {nfc_polling_interval}ms may cause protocol timeouts',
                'recommendation': 'Test with real tags, increase to 75ms if failures occur'
            })
        
        if len(nfc_errors) > 0:
            self.issues.append({
                'severity': 'MEDIUM',
                'issue': f'NFC Errors Detected ({len(nfc_errors)})',
                'description': 'NFC communication had errors during initialization',
                'recommendation': 'Check NFC hardware connections, test with real tags'
            })
        
        self.metrics['nfc_polling_interval'] = nfc_polling_interval
        self.metrics['nfc_detections'] = len(nfc_detections)
        self.metrics['nfc_errors'] = len(nfc_errors)
    
    def check_spi_speed(self):
        """Verify SPI speed configuration"""
        print("\n[5] CHECKING SPI SPEED CONFIGURATION...")
        
        spi_speeds = {}
        
        for line in self.logs:
            if 'BOARD_LCD_SPI_FREQ' in line or 'SPI_FREQ' in line:
                matches = re.findall(r'(\d+)\s*[MG]Hz|(\d+)\s*\*\s*1000\s*\*\s*1000', line)
                for match in matches:
                    freq_mhz = int(match[0] or match[1]) // (1000000 if len(match[1]) > 6 else 1)
                    if freq_mhz > 10:  # Filter out small numbers
                        if 'LCD' in line or 'DISPLAY' in line:
                            spi_speeds['LCD'] = freq_mhz
                        elif 'SD' in line:
                            spi_speeds['SD'] = freq_mhz
                        elif 'CC1101' in line:
                            spi_speeds['CC1101'] = freq_mhz
                        elif 'NRF24' in line:
                            spi_speeds['NRF24'] = freq_mhz
        
        for device, speed in spi_speeds.items():
            print(f"  {device}: {speed} MHz")
        
        # Check for SPI bus contention risks
        if spi_speeds.get('LCD', 0) >= 40 and spi_speeds.get('SD', 0) >= 30:
            self.issues.append({
                'severity': 'HIGH',
                'issue': 'Potential SPI Bus Contention',
                'description': f"LCD at {spi_speeds.get('LCD')}MHz, SD at {spi_speeds.get('SD')}MHz on shared bus",
                'recommendation': 'Implement SPI speed switching per device or reduce to 30MHz'
            })
        
        self.metrics['spi_speeds'] = spi_speeds
    
    def check_memory_pools(self):
        """Verify memory pool initialization"""
        print("\n[6] CHECKING MEMORY POOLS...")
        
        pool_initialized = False
        pool_sizes = {}
        allocation_failures = []
        
        for line in self.logs:
            if 'memory pool' in line.lower() and 'init' in line.lower():
                pool_initialized = True
                print("  Memory pool initialized")
            
            if 'pool allocated' in line.lower() or 'bytes' in line.lower():
                match = re.search(r'(\d+)\s*bytes', line)
                if match:
                    size = int(match.group(1))
                    if 'DMA' in line:
                        pool_sizes['DMA'] = size
                    elif 'PSRAM' in line:
                        pool_sizes['PSRAM'] = size
                    elif 'animation' in line.lower():
                        pool_sizes['Animation'] = size
            
            if 'allocation' in line.lower() and ('fail' in line.lower() or 'error' in line.lower()):
                allocation_failures.append(line.strip())
        
        for pool, size in pool_sizes.items():
            print(f"  {pool} Pool: {size / 1024:.1f} KB")
        
        if len(allocation_failures) > 0:
            self.issues.append({
                'severity': 'MEDIUM',
                'issue': f'Memory Allocation Failures ({len(allocation_failures)})',
                'description': 'Pre-allocated memory pools insufficient for operations',
                'recommendation': 'Increase pool sizes or implement fallback allocation'
            })
        
        if not pool_initialized:
            self.issues.append({
                'severity': 'MEDIUM',
                'issue': 'Memory Pool Not Initialized',
                'description': 'Memory optimization pools may not be active',
                'recommendation': 'Verify furi_memory_pool_init() called during boot'
            })
        
        self.metrics['pool_sizes'] = pool_sizes
        self.metrics['allocation_failures'] = len(allocation_failures)
    
    def check_animation_loading(self):
        """Verify deferred animation loading"""
        print("\n[7] CHECKING ANIMATION LOADING...")
        
        animation_load_time = None
        animation_errors = []
        
        for i, line in enumerate(self.logs):
            if 'animation' in line.lower() and 'load' in line.lower():
                if 'error' in line.lower() or 'fail' in line.lower():
                    animation_errors.append(line.strip())
                else:
                    match = re.search(r'(\d+)\s*ms', line)
                    if match:
                        animation_load_time = int(match.group(1))
        
        print(f"  Animation Load Time: {animation_load_time or 'Not detected'} ms")
        print(f"  Animation Errors: {len(animation_errors)}")
        
        if len(animation_errors) > 0:
            self.issues.append({
                'severity': 'LOW',
                'issue': f'Animation Loading Errors ({len(animation_errors)})',
                'description': 'Some animations failed to load during startup',
                'recommendation': 'Check animation files on SD card for corruption'
            })
        
        if animation_load_time and animation_load_time > 500:
            self.issues.append({
                'severity': 'LOW',
                'issue': 'Animation Loading Slow',
                'description': f'Animation load took {animation_load_time}ms (deferred should be <500ms)',
                'recommendation': 'Check SD card speed and animation file count'
            })
        
        self.metrics['animation_load_time'] = animation_load_time
        self.metrics['animation_errors'] = len(animation_errors)
    
    def check_crash_indicators(self):
        """Detect crash and error patterns"""
        print("\n[8] CHECKING CRASH INDICATORS...")
        
        crash_indicators = [
            'Guru Meditation Error',
            'backtrace:',
            'ASSERT',
            'segmentation fault',
            'stack overflow',
            'Unhandled exception',
            'abort()',
            'FATAL'
        ]
        
        crashes = []
        for line in self.logs:
            for indicator in crash_indicators:
                if indicator.lower() in line.lower():
                    crashes.append(line.strip())
        
        if crashes:
            self.issues.append({
                'severity': 'CRITICAL',
                'issue': f'Crashes Detected ({len(crashes)})',
                'description': f'Found {len(crashes)} crash/error indicators in logs',
                'recommendation': 'Review crash logs, enable debug output, test hardware'
            })
            print(f"  ⚠ Found {len(crashes)} crash indicators")
        else:
            print("  ✓ No crash indicators detected")
        
        self.metrics['crashes'] = len(crashes)
    
    def check_optimization_integrity(self):
        """Verify all optimizations are active"""
        print("\n[9] CHECKING OPTIMIZATION INTEGRITY...")
        
        optimizations = {
            'Service Delay Reduced': r'furi_delay_ms\(1\)',
            'SD Async Mount': r'record_create.*storage',
            'Stripe Height 16': r'STRIPE_HEIGHT\s*16',
            'Frame Rate Cap 60': r'GUI_FRAME_TIME|60.*FPS',
            'NFC Polling 50ms': r'ISO14443.*50.*ms',
            'SPI 40MHz': r'40.*1000.*1000|40.*MHz',
            'Memory Pools': r'furi_memory_pool|MALLOC_CAP_DMA',
            'Deferred Animation': r'animation.*deferred|animation.*background'
        }
        
        active_optimizations = []
        for opt_name, pattern in optimizations.items():
            found = any(re.search(pattern, line) for line in self.logs)
            status = "✓" if found else "✗"
            print(f"  {status} {opt_name}")
            if found:
                active_optimizations.append(opt_name)
        
        if len(active_optimizations) < len(optimizations) // 2:
            self.issues.append({
                'severity': 'HIGH',
                'issue': 'Optimizations Not Fully Active',
                'description': f'Only {len(active_optimizations)}/{len(optimizations)} optimizations found in logs',
                'recommendation': 'Rebuild firmware with optimization flags enabled'
            })
        
        self.metrics['active_optimizations'] = len(active_optimizations)
        self.metrics['total_optimizations'] = len(optimizations)
    
    def generate_report(self):
        """Generate comprehensive audit report"""
        print("\n" + "="*60)
        print("REAPER FURY AUDIT REPORT")
        print("="*60)
        
        # Run all checks
        self.check_boot_sequence()
        self.check_sd_mount_race_condition()
        self.check_display_rendering()
        self.check_nfc_performance()
        self.check_spi_speed()
        self.check_memory_pools()
        self.check_animation_loading()
        self.check_crash_indicators()
        self.check_optimization_integrity()
        
        # Summary
        print("\n" + "="*60)
        print("ISSUE SUMMARY")
        print("="*60)
        
        if not self.issues:
            print("✓ NO ISSUES DETECTED")
        else:
            critical = [i for i in self.issues if i['severity'] == 'CRITICAL']
            high = [i for i in self.issues if i['severity'] == 'HIGH']
            medium = [i for i in self.issues if i['severity'] == 'MEDIUM']
            low = [i for i in self.issues if i['severity'] == 'LOW']
            
            print(f"Critical: {len(critical)}")
            print(f"High: {len(high)}")
            print(f"Medium: {len(medium)}")
            print(f"Low: {len(low)}")
            
            for issue in self.issues:
                print(f"\n[{issue['severity']}] {issue['issue']}")
                print(f"  Description: {issue['description']}")
                print(f"  Recommendation: {issue['recommendation']}")
        
        # Metrics
        print("\n" + "="*60)
        print("PERFORMANCE METRICS")
        print("="*60)
        for metric, value in self.metrics.items():
            print(f"{metric}: {value}")
        
        print("\n" + "="*60)
        print("Report generated:", datetime.now().isoformat())
        print("="*60)

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print("Usage: python3 audit_reaper_fury.py <log_file>")
        print("Example: python3 audit_reaper_fury.py device.log")
        sys.exit(1)
    
    log_file = sys.argv[1]
    auditor = ReaperFuryAuditor(log_file)
    auditor.generate_report()
