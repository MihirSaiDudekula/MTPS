#!/usr/bin/env python3
'''
Enhanced Test Suite for Multithreaded Proxy Server

This script performs comprehensive testing of the proxy server and backend server,
measuring performance metrics and validating functionality with improved error handling
and visualization. It supports both direct and proxied requests to compare performance.

Key Features:
- Concurrent request testing
- Performance metrics collection
- Visualization of results
- Error handling and logging
- Support for various HTTP methods
'''

# Standard library imports
import sys
import time
import json
import socket
import logging
import statistics
import argparse
from pathlib import Path
from datetime import datetime
from typing import Dict, List, Any, Optional, Tuple
import concurrent.futures

# Third-party imports
import requests  # For making HTTP requests
import pandas as pd  # For data manipulation and analysis
import numpy as np  # For numerical operations
import matplotlib.pyplot as plt  # For creating static visualizations
import seaborn as sns  # For statistical data visualization

# Configure logging to both file and console
# This helps in debugging and monitoring the test execution

def setup_logging():
    """
    Configure logging for the application.
    
    Sets up logging to both console and file with a specific format.
    Logs are written to 'performance_test.log' in the current directory.
    """
    logging.basicConfig(
        level=logging.INFO,
        format='%(asctime)s - %(levelname)s - %(message)s',
        handlers=[
            logging.FileHandler('performance_test.log'),
            logging.StreamHandler()
        ]
    )
    return logging.getLogger(__name__)

# Initialize logger
logger = setup_logging()

class PerformanceTester:
    """
    A class to test and compare performance of direct vs proxied HTTP requests.
    
    This class provides methods to:
    - Make HTTP requests to both direct and proxy endpoints
    - Collect performance metrics
    - Generate reports and visualizations
    - Handle concurrent requests
    """
    
    def __init__(self, server_url: str, proxy_url: str, output_dir: str = "results"):
        """
        Initialize the PerformanceTester with server and proxy URLs.
        
        Args:
            server_url (str): Base URL of the backend server (e.g., 'http://localhost:3000')
            proxy_url (str): Base URL of the proxy server (e.g., 'http://localhost:8080')
            output_dir (str): Directory to save test results and graphs (default: 'results')
        """
        # Remove trailing slashes from URLs for consistency
        self.server_url = server_url.rstrip('/')
        self.proxy_url = proxy_url.rstrip('/')
        
        # Set up output directory
        self.output_dir = Path(output_dir)
        self.output_dir.mkdir(exist_ok=True)
        
        # Initialize data structure to store performance metrics
        # This will be used to generate reports and visualizations
        self.performance_data = {
            'operation': [],     # The API endpoint being tested
            'target': [],        # 'direct' or 'proxy' - indicates request path
            'response_time': [], # Response time in milliseconds
            'status_code': [],   # HTTP status code of the response
            'timestamp': [],     # When the request was made
            'cache_hit': [],     # Whether the response came from cache
            'data_size': []      # Size of response data in bytes
        }
        
    def _make_request(self, url: str, method: str = 'GET', 
                     data: Optional[dict] = None, 
                     headers: Optional[dict] = None,
                     target: str = 'direct') -> Tuple[float, int, bool, bool, int]:
        """
        Execute an HTTP request and capture performance metrics.
        
        This internal method handles the actual HTTP request execution, timing, and error handling.
        It supports GET, POST, and DELETE methods and captures various performance metrics.
        
        Args:
            url (str): The complete URL to send the request to
            method (str): HTTP method to use (GET, POST, DELETE)
            data (dict, optional): JSON data to send with POST requests
            headers (dict, optional): Custom HTTP headers to include in the request
            target (str): 'direct' or 'proxy' - used for logging and metrics
            
        Returns:
            Tuple containing:
                - response_time_ms (float): Time taken for the request in milliseconds
                - status_code (int): HTTP status code (0 if request failed)
                - success (bool): Whether the request was successful
                - cache_hit (bool): Whether the response came from cache
                - data_size (int): Size of the response data in bytes
                
        Note:
            - Times out after 10 seconds if no response is received
            - Logs errors but doesn't raise exceptions to allow test continuation
        """
        start_time = time.time()
        cache_hit = False
        data_size = 0
        
        try:
            # Execute the appropriate HTTP method
            method = method.upper()
            if method == 'GET':
                response = requests.get(url, headers=headers, timeout=10)
            elif method == 'POST':
                response = requests.post(url, json=data, headers=headers, timeout=10)
            elif method == 'DELETE':
                response = requests.delete(url, headers=headers, timeout=10)
            else:
                raise ValueError(f"Unsupported HTTP method: {method}")
                
            # Calculate response time in milliseconds
            response_time = (time.time() - start_time) * 1000
            
            # Get response metadata
            data_size = len(response.content)
            cache_hit = response.headers.get('X-Cache', '').lower() == 'hit'
            
            return (
                response_time,          # Time taken in milliseconds
                response.status_code,   # HTTP status code
                response.ok,            # True for 2xx status codes
                cache_hit,              # Whether response came from cache
                data_size               # Response body size in bytes
            )
            
        except requests.exceptions.RequestException as e:
            # Handle request errors gracefully
            response_time = (time.time() - start_time) * 1000
            logger.error(f"Request to {url} failed: {str(e)}")
            return (response_time, 0, False, False, 0)
    
    def _log_request(self, operation: str, target: str, response_time: float,
                    status_code: int, cache_hit: bool, data_size: int) -> None:
        """
        Record request metrics in the performance data store.
        
        This method stores the results of each request in a structured format
        for later analysis and reporting.
        
        Args:
            operation (str): The API endpoint or operation being tested
            target (str): 'direct' or 'proxy' - indicates request path
            response_time (float): Time taken for the request in milliseconds
            status_code (int): HTTP status code of the response
            cache_hit (bool): Whether the response came from cache
            data_size (int): Size of the response data in bytes
        """
        # Log the request details at DEBUG level
        logger.debug(f"Request logged - Operation: {operation}, Target: {target}, "
                   f"Status: {status_code}, Time: {response_time:.2f}ms, "
                   f"Cache: {'hit' if cache_hit else 'miss'}, Size: {data_size} bytes")
        
        # Store the metrics in the performance data dictionary
        self.performance_data['operation'].append(operation)
        self.performance_data['target'].append(target)
        self.performance_data['response_time'].append(response_time)
        self.performance_data['status_code'].append(status_code)
        self.performance_data['timestamp'].append(datetime.now())
        self.performance_data['cache_hit'].append(cache_hit)
        self.performance_data['data_size'].append(data_size)
    
    def test_endpoint(self, endpoint: str, method: str = 'GET',
                     data: Optional[dict] = None, num_requests: int = 10,
                     concurrent: bool = True) -> Dict[str, Any]:
        """
        Test a specific API endpoint through both direct and proxy connections.
        
        This method performs load testing on the specified endpoint by sending multiple
        requests through both direct and proxy paths, then collects and returns performance metrics.
        
        Args:
            endpoint (str): The API endpoint to test (e.g., 'users', 'health')
            method (str): HTTP method to use (GET, POST, etc.)
            data (dict, optional): Request payload for POST/PUT methods
            num_requests (int): Number of requests to send per target (direct/proxy)
            concurrent (bool): Whether to run tests concurrently for better performance
            
        Returns:
            dict: A dictionary containing detailed performance statistics for both
                  direct and proxy requests, including:
                  - avg_time: Average response time in milliseconds
                  - min_time: Minimum response time
                  - max_time: Maximum response time
                  - p90: 90th percentile response time
                  - success_rate: Percentage of successful requests
                  - throughput: Requests per second
        """
        logger.info(f"Testing endpoint: {endpoint} ({method}) - {num_requests} requests per target"
                  f" (concurrent: {concurrent})")
        
        # Initialize results storage
        results = {
            'direct': {'times': [], 'success': 0, 'total': 0},
            'proxy': {'times': [], 'success': 0, 'total': 0}
        }
        
        # Construct full URLs for both direct and proxy requests
        urls = {
            'direct': f"{self.server_url}/{endpoint.lstrip('/')}",
            'proxy': f"{self.proxy_url}/{endpoint.lstrip('/')}"
        }
        
        def _test_single(target: str) -> None:
            """
            Inner function to test a single target (direct or proxy).
            
            Args:
                target (str): 'direct' or 'proxy' - indicates which URL to test
            """
            logger.debug(f"Testing {target} connection to {urls[target]}")
            
            for i in range(num_requests):
                # Make the HTTP request and capture metrics
                response_time, status_code, success, cache_hit, data_size = self._make_request(
                    urls[target], method, data, target=target
                )
                
                # Log the request details
                self._log_request(
                    operation=endpoint,
                    target=target,
                    response_time=response_time,
                    status_code=status_code,
                    cache_hit=cache_hit,
                    data_size=data_size
                )
                
                # Update results
                results[target]['times'].append(response_time)
                results[target]['success'] += 1 if success else 0
                results[target]['total'] += 1
                
                # Log progress for long-running tests
                if (i + 1) % 10 == 0 or (i + 1) == num_requests:
                    logger.debug(f"Completed {i+1}/{num_requests} requests to {target}")
        
        # Execute tests either concurrently or sequentially based on the concurrent flag
        if concurrent:
            logger.info("Running concurrent tests...")
            with concurrent.futures.ThreadPoolExecutor(max_workers=10) as executor:
                # Create a future for each test run
                futures = [
                    executor.submit(_test_single, target)
                    for target in ['direct', 'proxy']
                    for _ in range(num_requests)
                ]
                # Wait for all futures to complete
                concurrent.futures.wait(futures)
                
                # Check for any exceptions in the completed futures
                for future in futures:
                    try:
                        future.result()  # This will raise any exceptions that occurred
                    except Exception as e:
                        logger.error(f"Error in concurrent test: {str(e)}")
        else:
            logger.info("Running sequential tests...")
            _test_single('direct')
            _test_single('proxy')
        
        # Calculate comprehensive statistics for the test run
        stats = {}
        for target in ['direct', 'proxy']:
            if results[target]['times']:
                times = results[target]['times']
                success_rate = (results[target]['success'] / results[target]['total']) * 100
                total_time = sum(times) / 1000  # Convert to seconds for throughput calculation
                
                stats[target] = {
                    'avg_time': statistics.mean(times),  # Average response time in ms
                    'min_time': min(times),             # Fastest response time in ms
                    'max_time': max(times),             # Slowest response time in ms
                    'p90': np.percentile(times, 90),    # 90th percentile response time in ms
                    'success_rate': success_rate,        # Percentage of successful requests
                    'throughput': len(times) / total_time if total_time > 0 else 0,  # req/s
                    'total_requests': len(times),        # Total number of requests made
                    'failed_requests': len(times) - results[target]['success']  # Failed requests
                }
                
                # Log summary for this target
                logger.info(
                    f"{target.upper()} Results - "
                    f"Avg: {stats[target]['avg_time']:.2f}ms, "
                    f"Min: {stats[target]['min_time']:.2f}ms, "
                    f"Max: {stats[target]['max_time']:.2f}ms, "
                    f"Success: {stats[target]['success_rate']:.1f}%"
                )
        
        return stats
    
    def generate_report(self) -> None:
        """
        Generate comprehensive performance reports and visualizations.
        
        This method processes the collected performance data to create:
        1. Raw data CSV files
        2. Summary statistics
        3. Visualizations (time series, box plots, etc.)
        
        The generated files are saved to the output directory specified during initialization.
        """
        if not self.performance_data['operation']:
            logger.warning("No performance data available to generate report")
            return
        
        logger.info("Generating performance report...")
        
        try:
            # Convert performance data to a pandas DataFrame for easier analysis
            df = pd.DataFrame(self.performance_data)
            
            # Ensure output directory exists
            self.output_dir.mkdir(exist_ok=True)
            
            # 1. Save raw performance data
            raw_data_path = self.output_dir / 'performance_data.csv'
            df.to_csv(raw_data_path, index=False)
            logger.info(f"Saved raw performance data to: {raw_data_path}")
            
            # 2. Generate and save summary statistics
            summary = df.groupby(['operation', 'target'])['response_time'].agg(
                mean=('response_time', 'mean'),
                min=('response_time', 'min'),
                max=('response_time', 'max'),
                p50=('response_time', lambda x: np.percentile(x, 50)),
                p90=('response_time', lambda x: np.percentile(x, 90)),
                p95=('response_time', lambda x: np.percentile(x, 95)),
                std=('response_time', 'std'),
                count=('response_time', 'count'),
                success_rate=('status_code', lambda x: (x < 400).mean() * 100)
            ).reset_index()
            
            # Format the summary for better readability
            summary = summary.round({
                'mean': 2, 'min': 2, 'max': 2, 'p50': 2, 'p90': 2, 'p95': 2, 'std': 2, 'success_rate': 2
            })
            
            summary_path = self.output_dir / 'summary_statistics.csv'
            summary.to_csv(summary_path, index=False)
            logger.info(f"Saved summary statistics to: {summary_path}")
            
            # 3. Generate visualizations if we have enough data
            self._generate_visualizations(df, summary)
            
            logger.info("Report generation completed successfully")
            
        except Exception as e:
            logger.error(f"Error generating report: {str(e)}")
            raise
        
        # Generate visualizations
        self._generate_plots(df)
        
        # Print summary
        print("\n" + "="*50)
        print("PERFORMANCE TEST SUMMARY")
        print("="*50)
        print(summary.to_string())
        print("\nDetailed results saved to:", self.output_dir.absolute())
    
    def _generate_plots(self, df: pd.DataFrame) -> None:
        """Generate performance visualization plots."""
        plt.figure(figsize=(15, 10))
        
        # Response time comparison
        plt.subplot(2, 2, 1)
        sns.boxplot(x='operation', y='response_time', hue='target', data=df)
        plt.title('Response Time Comparison')
        plt.yscale('log')
        plt.xticks(rotation=45)
        
        # Success rate
        plt.subplot(2, 2, 2)
        success_rates = df.groupby(['operation', 'target'])['status_code'].apply(
            lambda x: (x < 400).mean() * 100
        ).unstack()
        success_rates.plot(kind='bar', ax=plt.gca())
        plt.title('Success Rate by Endpoint')
        plt.ylabel('Success Rate (%)')
        plt.xticks(rotation=45)
        
        # Throughput
        plt.subplot(2, 2, 3)
        throughput = df.groupby(['operation', 'target']).size().unstack()
        throughput.plot(kind='bar', ax=plt.gca())
        plt.title('Total Requests by Endpoint')
        plt.ylabel('Number of Requests')
        plt.xticks(rotation=45)
        
        # Data size distribution
        plt.subplot(2, 2, 4)
        sns.violinplot(x='operation', y='data_size', hue='target', data=df)
        plt.title('Response Size Distribution')
        plt.yscale('log')
        plt.ylabel('Response Size (bytes)')
        plt.xticks(rotation=45)
        
        stats[target] = {
            'avg_time': statistics.mean(times),  # Average response time in ms
            'min_time': min(times),             # Fastest response time in ms
            'max_time': max(times),             # Slowest response time in ms
            'p90': np.percentile(times, 90),    # 90th percentile response time in ms
            'success_rate': success_rate,        # Percentage of successful requests
            'throughput': len(times) / total_time if total_time > 0 else 0,  # req/s
            'total_requests': len(times),        # Total number of requests made
            'failed_requests': len(times) - results[target]['success']  # Failed requests
        }
        
        # Log summary for this target
        logger.info(
            f"{target.upper()} Results - "
            f"Avg: {stats[target]['avg_time']:.2f}ms, "
            f"Min: {stats[target]['min_time']:.2f}ms, "
            f"Max: {stats[target]['max_time']:.2f}ms, "
            f"Success: {stats[target]['success_rate']:.1f}%"
        )

def generate_report(self) -> None:
    """
    Generate comprehensive performance reports and visualizations.
    
    This method processes the collected performance data to create:
    1. Raw data CSV files
    2. Summary statistics
    3. Visualizations (time series, box plots, etc.)
    
    The generated files are saved to the output directory specified during initialization.
    """
    if not self.performance_data['operation']:
        logger.warning("No performance data available to generate report")
        return
    
    logger.info("Generating performance report...")
    
    try:
        # Convert performance data to a pandas DataFrame for easier analysis
        df = pd.DataFrame(self.performance_data)
        
        # Ensure output directory exists
        self.output_dir.mkdir(exist_ok=True)
        
        # 1. Save raw performance data
        raw_data_path = self.output_dir / 'performance_data.csv'
        df.to_csv(raw_data_path, index=False)
        logger.info(f"Saved raw performance data to: {raw_data_path}")
        
        # 2. Generate and save summary statistics
        summary = df.groupby(['operation', 'target'])['response_time'].agg(
            mean=('response_time', 'mean'),
            min=('response_time', 'min'),
            max=('response_time', 'max'),
            p50=('response_time', lambda x: np.percentile(x, 50)),
            p90=('response_time', lambda x: np.percentile(x, 90)),
            p95=('response_time', lambda x: np.percentile(x, 95)),
            std=('response_time', 'std'),
            count=('response_time', 'count'),
            success_rate=('status_code', lambda x: (x < 400).mean() * 100)
        ).reset_index()
        
        # Format the summary for better readability
        summary = summary.round({
            'mean': 2, 'min': 2, 'max': 2, 'p50': 2, 'p90': 2, 'p95': 2, 'std': 2, 'success_rate': 2
        })
        
        summary_path = self.output_dir / 'summary_statistics.csv'
        summary.to_csv(summary_path, index=False)
        logger.info(f"Saved summary statistics to: {summary_path}")
        
        # 3. Generate visualizations if we have enough data
        self._generate_visualizations(df, summary)
        
        logger.info("Report generation completed successfully")
        
    except Exception as e:
        logger.error(f"Error generating report: {str(e)}")
        raise
        
    # Generate visualizations
    self._generate_plots(df)
    
    # Print summary
    print("\n" + "="*50)
    print("PERFORMANCE TEST SUMMARY")
    print("="*50)
    print(summary.to_string())
    print("\nDetailed results saved to:", self.output_dir.absolute())

def parse_arguments():
    """
    Parse and validate command line arguments.
    
    Returns:
        argparse.Namespace: Parsed command line arguments with the following attributes:
            - server (str): Base URL of the backend server
            - proxy (str): Base URL of the proxy server
            - output (str): Output directory for test results
            
    Example:
        $ python script.py --server http://localhost:3000 --proxy http://localhost:8080 --output results
    """
    parser = argparse.ArgumentParser(
        description='Performance Testing Tool for Proxy Server Evaluation',
        formatter_class=argparse.ArgumentDefaultsHelpFormatter
    )
    
    # Server configuration arguments
    parser.add_argument(
        '--server', 
        type=str, 
        default='http://localhost:3000',
        help='Base URL of the backend server (e.g., http://localhost:3000)'
    )
    
    parser.add_argument(
        '--proxy', 
        type=str, 
        default='http://localhost:8080',
        help='Base URL of the proxy server (e.g., http://localhost:8080)'
    )
    
    # Output configuration
    parser.add_argument(
        '--output', 
        type=str, 
        default='results',
        help='Output directory for test results and reports'
    )
    
    # Parse and validate arguments
    args = parser.parse_args()
    
    # Basic URL validation
    for url in [args.server, args.proxy]:
        if not (url.startswith('http://') or url.startswith('https://')):
            parser.error(f"URL must start with 'http://' or 'https://': {url}")
    
    return args

def main():
    """
    Main entry point for the performance testing script.
    
    This function:
    1. Parses command line arguments
    2. Initializes the PerformanceTester
    3. Runs a series of tests against the server and proxy
    4. Generates reports and visualizations
    5. Handles errors and user interrupts gracefully
    
    The script can be run with various command line arguments to customize the test:
    $ python script.py --server http://localhost:3000 --proxy http://localhost:8080 --output results
    """
    try:
        # Parse command line arguments
        args = parse_arguments()
        
        # Initialize the tester with command line arguments
        tester = PerformanceTester(
            server_url=args.server,
            proxy_url=args.proxy,
            output_dir=args.output
        )
        
        # Print test configuration
        print("\n" + "="*60)
        print("PROXY PERFORMANCE TESTING TOOL".center(60))
        print("="*60)
        print(f"Backend Server: {args.server}")
        print(f"Proxy Server:   {args.proxy}")
        print(f"Output Dir:     {args.output}")
        print("-"*60)
        
        # Define test scenarios
        test_cases = [
            # Basic health check (lightweight)
            {'endpoint': 'health', 'method': 'GET', 'data': None, 'requests': 10},
            
            # Standard API endpoint
            {'endpoint': 'test', 'method': 'GET', 'data': None, 'requests': 20},
            
            # Large file download test
            {
                'endpoint': f'large?size=10',  # 10MB file
                'method': 'GET',
                'data': None,
                'requests': 3,
                'concurrent': False
            },
            
            # POST request with data
            {
                'endpoint': 'users', 
                'method': 'POST', 
                'data': {
                    'id': 'test_user_' + str(int(time.time())),
                    'name': 'Test User',
                    'email': f'test_{int(time.time())}@example.com'
                },
                'requests': 15
            },
            
            # Large data endpoint (tests throughput)
            {'endpoint': 'large', 'method': 'GET', 'data': None, 'requests': 5}
        ]
        
        # Execute each test case
        for i, test in enumerate(test_cases, 1):
            print(f"\nTest {i}/{len(test_cases)}: {test['method']} {test['endpoint']}")
            print("-" * 40)
            
            tester.test_endpoint(
                endpoint=test['endpoint'],
                method=test['method'],
                data=test['data'],
                num_requests=test['requests'],
                concurrent=args.concurrent
            )
        
        # Generate comprehensive report
        print("\n" + "="*60)
        print("GENERATING FINAL REPORT".center(60))
        print("="*60)
        tester.generate_report()
        
        print("\n" + "="*60)
        print("TESTING COMPLETED SUCCESSFULLY".center(60))
        print("="*60)
        print(f"Results saved to: {Path(args.output).resolve()}")
        
    except KeyboardInterrupt:
        print("\n\nTest interrupted by user. Partial results will be saved.")
        if 'tester' in locals():
            tester.generate_report()  # Save partial results
        sys.exit(1)
        
    except Exception as e:
        print(f"\n\nError during testing: {str(e)}")
        logging.error("Unhandled exception", exc_info=True)
        sys.exit(1)
    
    # Generate final report
    tester.generate_report()

if __name__ == "__main__":
    main()

def test_large_request(server_url: str, proxy_url: str, num_requests: int = 3, size_mb: int = 10) -> list:
    """
    Test the performance of large file downloads through both direct and proxy connections.
    
    This function is specifically designed to measure:
    - Download speeds for large files
    - Memory usage during large transfers
    - Caching effectiveness for large responses
    
    Args:
        server_url (str): Base URL of the backend server
        proxy_url (str): Base URL of the proxy server
        num_requests (int): Number of times to repeat the test (default: 3)
        size_mb (int): Size of the test file in megabytes (default: 10MB)
        
    Returns:
        list: A list of dictionaries containing test results
        
    The test will download the specified file multiple times and report statistics
    for both direct and proxied connections.
    """
    print(f"\n{'='*60}")
    print(f"LARGE FILE DOWNLOAD TEST ({size_mb}MB)".center(60))
    print(f"{'='*60}")
    
    # Initialize the tester with a dedicated output directory
    tester = PerformanceTester(
        server_url=server_url,
        proxy_url=proxy_url,
        output_dir=f"large_request_{size_mb}MB_results"
    )
    
    # Track overall statistics
    all_stats = []
    
    try:
        # Run the test multiple times to get consistent measurements
        for i in range(num_requests):
            print(f"\n--- Test Iteration {i+1}/{num_requests} ---")
            print(f"Downloading {size_mb}MB file...")
            
            # Time the download
            start_time = time.time()
            
            # Execute the test
            stats = tester.test_endpoint(
                endpoint=f"large?size={size_mb}",
                method='GET',
                num_requests=1,
                concurrent=False  # Run sequentially for large downloads
            )
            
            # Calculate download speed
            for target, metrics in stats.items():
                duration_s = metrics['avg_time'] / 1000  # Convert ms to seconds
                size_mb_actual = metrics.get('data_size', 0) / (1024 * 1024)
                mbps = (size_mb_actual * 8) / duration_s if duration_s > 0 else 0
                
                print(f"\n{target.upper()} Results:")
                print(f"  Time: {metrics['avg_time']:,.2f} ms")
                print(f"  Size: {size_mb_actual:,.2f} MB")
                print(f"  Speed: {mbps:,.2f} Mbps")
                
                # Store for final summary
                all_stats.append({
                    'target': target,
                    'iteration': i + 1,
                    'time_ms': metrics['avg_time'],
                    'size_mb': size_mb_actual,
                    'speed_mbps': mbps
                })
    
    except Exception as e:
        print(f"\nError during large request test: {str(e)}")
        logging.error("Large request test failed", exc_info=True)
    
    finally:
        # Always generate a report, even if the test was interrupted
        print("\nGenerating final report...")
        tester.generate_report()
        
        # Print final summary
        if all_stats:
            print("\n" + "="*60)
            print("LARGE FILE DOWNLOAD SUMMARY".center(60))
            print("="*60)
            
            # Group by target (direct/proxy)
            import pandas as pd
            df = pd.DataFrame(all_stats)
            
            for target in ['direct', 'proxy']:
                target_stats = df[df['target'] == target]
                if len(target_stats) > 0:
                    print(f"\n{target.upper()} AVERAGES (n={len(target_stats)}):")
                    print(f"  Time: {target_stats['time_ms'].mean():,.2f} ms")
                    print(f"  Size: {target_stats['size_mb'].mean():,.2f} MB")
                    print(f"  Speed: {target_stats['speed_mbps'].mean():,.2f} Mbps")
                    print(f"  Min Speed: {target_stats['speed_mbps'].min():,.2f} Mbps")
                    print(f"  Max Speed: {target_stats['speed_mbps'].max():,.2f} Mbps")
            # Print divider between target types
            print("\n" + "-" * 60)
    
    return all_stats

def create_test_users(num_users=5):
    print(f"\nCreating {num_users} test users...")
    created_users = []
    
    for i in range(num_users):
        user_data = {
            "id": str(i + 1),
            "name": f"Test User {i + 1}",
            "email": f"user{i + 1}@example.com"
        }
        headers = {'Content-Type': 'application/json'}
        
        start_time = time.time()
        response = requests.post(
            f"{SERVER_URL}/users",
            json=user_data,
            headers=headers
        )
        elapsed_time = time.time() - start_time
        log_request('create_user', elapsed_time, response.status_code)
        
        if response.status_code == 201:
            created_users.append(response.json())
            print(f"Created user {i + 1}")
    
    return created_users

def get_users():
    print("\nRetrieving users through proxy...")
    headers = {'Host': 'localhost:3000'}
    
    start_time = time.time()
    response = requests.get(f"{PROXY_URL}/users", headers=headers)
    elapsed_time = time.time() - start_time
    
    log_request('get_users', elapsed_time, response.status_code)
    
    if response.status_code == 200:
        users = response.json()
        print(f"Retrieved {len(users)} users")
        return users
    else:
        print(f"Failed to retrieve users. Status code: {response.status_code}")
        return []

#def delete_users(user_ids):
#    print(f"\nDeleting {len(user_ids)} users...")
#    for user_id in user_ids:
#        start_time = time.time()
#        response = requests.delete(f"{SERVER_URL}/users/{user_id}")
#        elapsed_time = time.time() - start_time
#        log_request('delete_user', elapsed_time, response.status_code)
#        print(f"Deleted user {user_id}")

def generate_performance_graphs():
    """Generate comprehensive performance visualization"""
    df = pd.DataFrame(performance_data)
    
    # Use a clean, built-in style
    plt.style.use('bmh')
    
    # Create multiple subplots
    fig, axes = plt.subplots(3, 2, figsize=(18, 15))
    fig.suptitle('Proxy Server Performance Analysis', fontsize=16)
    
    # 1. Cache Performance Comparison
    cache_data = df[df['operation'].str.contains('large_request')]
    
    # Calculate statistics
    stats = cache_data.groupby('operation')['response_time'].agg(['mean', 'std']).reset_index()
    
    # Bar plot for cache performance
    ax = axes[0, 0]
    bars = ax.bar(
        ['First Request', 'Cached Requests'],
        stats['mean'],
        yerr=stats['std'],
        color=['lightcoral', 'lightgreen'],
        capsize=5
    )
    
    ax.set_title('Cache Performance Impact')
    ax.set_ylabel('Response Time (seconds)')
    
    # Add value labels
    for bar in bars:
        height = bar.get_height()
        ax.text(bar.get_x() + bar.get_width()/2., height,
                f'{height:.2f}s',
                ha='center', va='bottom')
    
    # Calculate improvement
    nocache_time = stats[stats['operation'] == 'large_request_nocache']['mean'].iloc[0]
    cache_time = stats[stats['operation'] == 'large_request_cached']['mean'].iloc[0]
    improvement = ((nocache_time - cache_time) / nocache_time) * 100
    
    ax.text(0.5, -0.15, 
             f'Cache Performance Improvement: {improvement:.1f}%',
             ha='center',
             transform=ax.transAxes,
             fontsize=12,
             bbox=dict(facecolor='white', alpha=0.8, edgecolor='none', pad=5))

    # 2. Response Time Distribution
    ax = axes[0, 1]
    sns.boxplot(data=df, x='operation', y='response_time', ax=ax)
    ax.set_title('Response Time Distribution')
    ax.set_ylabel('Response Time (seconds)')
    ax.set_xticklabels(ax.get_xticklabels(), rotation=45, ha='right')

    # 3. Cache Hit Rate
    ax = axes[1, 0]
    cache_stats = df.groupby('cache_hit').size()
    cache_stats.plot(kind='pie', autopct='%.1f%%', ax=ax)
    ax.set_title('Cache Hit Rate')
    ax.set_ylabel('')

    # 4. Status Code Distribution
    ax = axes[1, 1]
    status_stats = df['status_code'].value_counts()
    status_stats.plot(kind='bar', ax=ax)
    ax.set_title('Status Code Distribution')
    ax.set_ylabel('Count')

    # 5. Request Timeline
    ax = axes[2, 0]
    df['timestamp'] = pd.to_datetime(df['timestamp'])
    df.set_index('timestamp', inplace=True)
    df['response_time'].plot(ax=ax)
    ax.set_title('Request Response Times Over Time')
    ax.set_ylabel('Response Time (seconds)')

    # 6. Error Rate
    ax = axes[2, 1]
    error_rate = df[df['status_code'] >= 400].groupby('operation').size() / df.groupby('operation').size() * 100
    error_rate.plot(kind='bar', ax=ax)
    ax.set_title('Error Rate by Operation')
    ax.set_ylabel('Error Rate (%)')
    
    plt.tight_layout(rect=[0, 0, 1, 0.95])
    plt.savefig('performance_analysis.png')
    print("\nPerformance analysis saved to 'performance_analysis.png'")
    for op in regular_ops['operation'].unique():
        grouped_data.append(regular_ops[regular_ops['operation'] == op]['response_time'].values)
        labels.append(op.replace('_', ' ').title())
    
    # Create box plot
    plt.boxplot(grouped_data, labels=labels)
    plt.title('Response Times by Operation Type (Excluding Large Requests)', fontsize=14, pad=20)
    plt.xticks(rotation=45, ha='right')
    plt.ylabel('Response Time (seconds)', fontsize=12)
    
    # Adjust layout
    plt.tight_layout(pad=3.0)
    plt.savefig('performance_analysis.png', bbox_inches='tight', dpi=300)
    print("\nPerformance graphs saved as 'performance_analysis.png'")

def print_performance_summary():
    """Print improved performance summary with detailed statistics"""
    df = pd.DataFrame(performance_data)
    print("\nPerformance Summary:")
    print("-" * 50)
    
    # Calculate cache performance with more detail
    cache_data = df[df['operation'].str.contains('large_request')]
    nocache_stats = cache_data[cache_data['operation'] == 'large_request_nocache']['response_time']
    cache_stats = cache_data[cache_data['operation'] == 'large_request_cached']['response_time']
    
    print(f"\nLarge Request (10MB) Performance Analysis:")
    print(f"First request (no cache): {nocache_stats.mean():.4f} seconds")
    print(f"Cached requests:")
    print(f"  - Average: {cache_stats.mean():.4f} seconds")
    print(f"  - Min: {cache_stats.min():.4f} seconds")
    print(f"  - Max: {cache_stats.max():.4f} seconds")
    print(f"  - Standard deviation: {cache_stats.std():.4f} seconds")
    print(f"Cache speedup: {((nocache_stats.mean() - cache_stats.mean()) / nocache_stats.mean() * 100):.1f}%")
    
    print("\nOther Operations (mean response times):")
    regular_ops = df[~df['operation'].str.contains('large_request')]
    summary = regular_ops.groupby('operation')['response_time'].agg(['count', 'mean', 'min', 'max', 'std'])
    summary.columns = ['Count', 'Mean', 'Min', 'Max', 'Std Dev']
    print(summary.round(4))

if __name__ == "__main__":
    main()
