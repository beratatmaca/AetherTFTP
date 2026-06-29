# ==========================================
# Stage 1: Build AetherTFTP
# ==========================================
FROM ubuntu:22.04 AS builder

# Prevent interactive prompts during package installation
ENV DEBIAN_FRONTEND=noninteractive

# Install build dependencies
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    qt6-base-dev \
    libgl1-mesa-dev \
    && rm -rf /var/lib/apt/lists/*

# Set up working directory
WORKDIR /app

# Copy source code
COPY . .

# Configure, build and test
RUN cmake -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
    && cmake --build build -j$(nproc) \
    && ctest --test-dir build --output-on-failure

# ==========================================
# Stage 2: Minimal Runtime Image
# ==========================================
FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

# Install runtime dependencies for Qt 6 Core, Network, Widgets and GUI
RUN apt-get update && apt-get install -y \
    libqt6core6 \
    libqt6network6 \
    libqt6widgets6 \
    libqt6gui6 \
    libgl1-mesa-glx \
    && rm -rf /var/lib/apt/lists/*

# Copy build binary from builder stage
COPY --from=builder /app/build/aethertftp /usr/local/bin/aethertftp

# Expose default TFTP port
EXPOSE 69/udp
EXPOSE 6969/udp

# Set binary entrypoint
ENTRYPOINT ["/usr/local/bin/aethertftp"]
CMD ["--server", "--port", "6969", "--dir", "/var/tftp"]
