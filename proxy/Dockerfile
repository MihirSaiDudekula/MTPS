# Use a base image with GCC and other build tools
FROM gcc:12.2.0

# Install required packages
RUN apt-get update && apt-get install -y \
    make \
    gdb \
    valgrind \
    && rm -rf /var/lib/apt/lists/*

# Create app directory
WORKDIR /usr/src/app

# Copy source files (maintain directory structure)
COPY . .

# Build the proxy server
RUN make

# Expose the port the proxy runs on
EXPOSE 8080

# Command to run the proxy server
CMD ["./proxy", "8080"]