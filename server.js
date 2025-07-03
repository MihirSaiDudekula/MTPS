/*
 * Express Server Implementation
 * 
 * This server provides RESTful API endpoints and acts as the backend for the proxy server.
 * It includes security features, rate limiting, and performance optimizations.
 */

const express = require('express');
const app = express();
const port = 3000;
const cors = require('cors');
const helmet = require('helmet');
const rateLimit = require('express-rate-limit');
const compression = require('compression');
const morgan = require('morgan');

/**
 * Security middleware
 * Provides various security headers and protections
 */
app.use(helmet());

/**
 * Rate limiting middleware
 * Prevents abuse by limiting the number of requests per IP
 */
const limiter = rateLimit({
    windowMs: 15 * 60 * 1000, // 15 minutes
    max: 100 // limit each IP to 100 requests per windowMs
});
app.use(limiter);

/**
 * Compression middleware
 * Compresses responses to reduce bandwidth usage
 */
app.use(compression());

/**
 * Logging middleware
 * Uses morgan to log all HTTP requests
 */
app.use(morgan('combined'));

// CORS configuration
const corsOptions = {
    origin: function (origin, callback) {
        if (!origin || origin === 'http://localhost:3000') {
            callback(null, true);
        } else {
            callback(new Error('Not allowed by CORS'));
        }
    },
    methods: ['GET', 'POST', 'PUT', 'DELETE'],
    allowedHeaders: ['Content-Type', 'Authorization']
};

// CORS middleware
app.use(cors(corsOptions));

// JSON body parser with size limit
app.use(express.json({ limit: '10mb' }));

// Test data store
let users = [];

// Health check endpoint
app.get('/health', (req, res) => {
    res.status(200).json({
        status: 'ok',
        timestamp: new Date().toISOString(),
        uptime: process.uptime(),
        memoryUsage: process.memoryUsage()
    });
});

// Root endpoint
app.get('/', (req, res) => {
    res.status(200).json({
        message: 'Welcome to the Express Server',
        endpoints: {
            health: '/health',
            test: '/test',
            users: '/users',
            large: '/large'
        }
    });
});

// Test endpoint with rate limiting
app.get('/test', (req, res, next) => {
    try {
        // Rate limiting
        const ip = req.ip;
        const user = req.headers['x-forwarded-for'] || req.connection.remoteAddress;
        
        res.status(200).json({
            message: 'Hello from test server!',
            timestamp: new Date().toISOString(),
            clientIp: ip,
            user: user
        });
    } catch (error) {
        next(error);
    }
});

// GET with query params
app.get('/users', (req, res, next) => {
    try {
        res.json(users);
    } catch (error) {
        next(error);
    }
});

// POST endpoint
app.post('/users', (req, res, next) => {
    try {
        const user = req.body;
        if (!user.id || !user.name || !user.email) {
            throw new Error('Invalid user data. Ensure id, name, and email are provided.');
        }
        users.push(user);
        res.status(201).json(user);
    } catch (error) {
        next(error);
    }
});

// DELETE endpoint
app.delete('/users/:id', (req, res, next) => {
    try {
        const id = req.params.id;
        const initialLength = users.length;
        users = users.filter(user => user.id !== id);
        if (users.length === initialLength) {
            throw new Error(`User with id ${id} not found.`);
        }
        res.status(200).send(`User ${id} deleted`);
    } catch (error) {
        next(error);
    }
});

// Large response endpoint with streaming
app.get('/large', (req, res, next) => {
    try {
        // Set response headers
        res.setHeader('Content-Type', 'application/octet-stream');
        res.setHeader('Cache-Control', 'no-cache');
        
        // Generate data in chunks
        const chunkSize = 1024 * 1024; // 1MB chunks
        const totalSize = 10 * chunkSize; // 10MB total
        let currentSize = 0;
        
        // Stream data
        const interval = setInterval(() => {
            if (currentSize >= totalSize) {
                clearInterval(interval);
                res.end();
                return;
            }
            
            const chunk = Buffer.alloc(chunkSize, String.fromCharCode(65 + Math.floor(currentSize / chunkSize)));
            res.write(chunk);
            currentSize += chunkSize;
        }, 100); // Send chunks every 100ms
        
        // Handle client disconnect
        req.on('close', () => {
            clearInterval(interval);
        });
    } catch (error) {
        next(error);
    }
});

// Global error handling middleware
app.use((err, req, res, next) => {
    console.error('Error:', {
        timestamp: new Date().toISOString(),
        method: req.method,
        path: req.path,
        error: err.stack,
        message: err.message,
        statusCode: err.statusCode || 500
    });

    // Don't expose error details in production
    const response = {
        timestamp: new Date().toISOString(),
        status: 'error',
        message: process.env.NODE_ENV === 'production' 
            ? 'An unexpected error occurred' 
            : err.message,
        path: req.path,
        method: req.method
    };

    res.status(err.statusCode || 500).json(response);
});

// Handle 404 errors
app.use((req, res, next) => {
    const error = new Error(`Not Found: ${req.originalUrl}`);
    error.statusCode = 404;
    next(error);
});

// Server startup
const server = app.listen(port, '0.0.0.0', () => {
    console.log(`Server running at http://0.0.0.0:${port}`);
    console.log('Environment:', process.env.NODE_ENV || 'development');
    console.log('Memory usage:', process.memoryUsage());
});

// Graceful shutdown
process.on('SIGTERM', () => {
    console.log('SIGTERM received. Shutting down gracefully...');
    server.close(() => {
        console.log('Server closed');
        process.exit(0);
    });
});

process.on('unhandledRejection', (reason, promise) => {
    console.error('Unhandled Rejection at:', promise, 'reason:', reason);
});

process.on('uncaughtException', (error) => {
    console.error('Uncaught Exception:', error);
});
