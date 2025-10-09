# Use a lightweight Ubuntu base image
FROM ubuntu:22.04

# Install dependencies: gcc, make, libcurl, cJSON, and other essentials
RUN apt-get update && apt-get install -y \
    gcc \
    make \
    pkg-config \
    libcurl4-openssl-dev \
    libcjson-dev \
    && rm -rf /var/lib/apt/lists/*

# Set working directory
WORKDIR /app

# Copy source files
COPY main.c .

# Compile the MCP server with cJSON and libcurl
RUN gcc main.c -o mcpserver $(pkg-config --cflags --libs libcjson) -lcurl

# Create a safe directory for file operations
RUN mkdir -p /app/data

# Expose port 8080
EXPOSE 8080

# Environment variable for the xAI API key (override at runtime)
ENV XAI_API_KEY=""

# Run the server
CMD ["./mcpserver"]