import subprocess
import time
import json

# Config
backend_base = "http://localhost:3000"
proxy_base = "http://localhost:8080"

endpoints = [
    {"path": "/health", "method": "GET"},
    {"path": "/", "method": "GET"},
    {"path": "/test", "method": "GET"},
    {"path": "/users", "method": "GET"},
    {"path": "/large", "method": "GET"}
]

# Test Data for POST
user_data = {
    "id": "123",
    "name": "Test User",
    "email": "test@example.com"
}

def run_curl(url, method="GET", data=None):
    """Run curl command and measure elapsed time."""
    cmd = ["curl", "-o", "/dev/null", "-s", "-w", "%{time_total}"]

    if method == "POST":
        cmd += ["-H", "Content-Type: application/json", "-d", json.dumps(data or {})]

    cmd.append(url)

    result = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        time_taken = float(result.stdout.strip())
    except ValueError:
        time_taken = -1  # in case of parse errors
    return time_taken

def test_endpoint(endpoint):
    """Test single endpoint across backend & proxy with caching."""
    path = endpoint["path"]
    method = endpoint.get("method", "GET")
    print(f"\n▶️ Testing {method} {path}")

    backend_url = backend_base + path
    proxy_url = proxy_base + path

    # Backend Direct
    backend_time = run_curl(backend_url, method, user_data if method == "POST" else None)

    # Proxy First Call (Cache Miss)
    proxy_time1 = run_curl(proxy_url, method, user_data if method == "POST" else None)

    # Proxy Second Call (Cache Hit)
    proxy_time2 = run_curl(proxy_url, method, user_data if method == "POST" else None)

    return {
        "endpoint": path,
        "method": method,
        "backend_time": backend_time,
        "proxy_time_first": proxy_time1,
        "proxy_time_second": proxy_time2
    }

def main():
    print("=== 🚀 Full Proxy Benchmark Report ===")
    all_results = []

    # Pre-populate /users for DELETE test later
    print("\n▶️ Setting up test user...")
    run_curl(backend_base + "/users", method="POST", data=user_data)

    for ep in endpoints:
        result = test_endpoint(ep)
        all_results.append(result)

    # Clean-up: Delete test user
    print("\n▶️ Cleaning up test user...")
    run_curl(backend_base + "/users/123", method="DELETE")

    # Report
    print("\n=== 📊 Benchmark Results ===")
    for result in all_results:
        speedup = (result["proxy_time_first"] / result["proxy_time_second"]) if result["proxy_time_second"] > 0 else 0
        print(f"\nEndpoint: {result['method']} {result['endpoint']}")
        print(f"- Backend Time       : {result['backend_time']:.4f} sec")
        print(f"- Proxy (1st, Miss)  : {result['proxy_time_first']:.4f} sec")
        print(f"- Proxy (2nd, Hit)   : {result['proxy_time_second']:.4f} sec")
        print(f"- Cache Speed-up     : {speedup:.1f}x")

if __name__ == "__main__":
    main()
