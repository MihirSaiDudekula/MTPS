# Multi-Protocol Proxy System

A distributed system with components in different languages:
- Python client
- C proxy server
- Node.js server

## Project Structure

```
.
├── client/               # Python client
│   ├── src/
│   │   └── client.py
│   └── requirements.txt
│
├── proxy/                # C proxy server
│   ├── src/
│   │   └── proxy.c
│   └── Makefile
│
├── server/               # Node.js server
│   ├── src/
│   │   └── server.js
│   └── package.json
│
├── docs/                 # Documentation
│   └── ...
│
├── shared/               # Shared resources
│   └── config/
│       ├── .env
│       └── .env.example
│
├── .gitignore
└── README.md
```

## Setup Instructions

### Python Client
1. Install Python 3.x
2. Install dependencies:
   ```bash
   cd client
   pip install -r requirements.txt
   ```
3. Run the client:
   ```bash
   python src/client.py
   ```

### C Proxy
1. Install gcc
2. Compile:
   ```bash
   cd proxy
   make
   ```
3. Run the proxy:
   ```bash
   ./proxy
   ```

### Node.js Server
1. Install Node.js
2. Install dependencies:
   ```bash
   cd server
   npm install
   ```
3. Run the server:
   ```bash
   node src/server.js
   ```

## Configuration

1. Copy the example environment file:
   ```bash
   cp shared/config/.env.example shared/config/.env
   ```
2. Update `.env` with your configuration

## Development

- Python: 3.x
- C: C99
- Node.js: See server/package.json
