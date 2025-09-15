# Use a lightweight Ubuntu base image
FROM ubuntu:22.04

# Install dependencies: gcc, make, libcurl, and other build essentials
RUN apt-get update && apt-get install -y \
    gcc \
    make \
    libcurl4-openssl-dev \
    && rm -rf /var/lib/apt/lists/*

# Set working directory
WORKDIR /app

# Copy source files
COPY main.c .

# Compile the MCP server
RUN gcc main.c -o mcpserver -lcurl

# Create a safe directory for file operations
RUN mkdir /app/data

# Expose port 8080
EXPOSE 8080

# Set environment variable for xAI API key (default empty, override at runtime)
ENV XAI_API_KEY=""

# Command to run the server
CMD ["./mcpserver"]