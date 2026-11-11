# Start with the latest Alpine base image for the build stage
FROM alpine:3.18.12 AS builder

# Install all the necessary dependencies required for the build process
# These include tools and libraries for building and compiling the source code
RUN apk update && apk add --no-cache \
    git \
    gcc \
    g++ \
    build-base \
    libtool \
    curl \
    c-ares \
    c-ares-dev \
    make \
    cmake \
    linux-headers \
    grpc \
    grpc-dev \
    protobuf \
    protobuf-dev \
    nlohmann-json

# Update the PATH environment variable to include the gRPC libraries and binaries
ENV LD_LIBRARY_PATH="/app/install/lib"
ENV PATH="/app/install/bin:$PATH"

# Display the updated PATH environment variable
RUN echo $PATH

# Get and install ArmoniK api into the image
WORKDIR /tmp
ARG API_VERSION
RUN git clone --depth 1 https://github.com/aneoconsulting/ArmoniK.Api.git -b "${API_VERSION}" && \
    cd ArmoniK.Api/packages/cpp && \
    mkdir -p /app/proto && \
    mkdir -p /armonik/api && \
    cp -r ../../Protos/V1/* /app/proto && \
    mkdir -p build/ && \
    cd build/ && \
    cmake "-DCMAKE_INSTALL_PREFIX=/armonik/api" "-DBUILD_TEST=OFF" "-DBUILD_CLIENT=ON" "-DBUILD_WORKER=OFF" .. && \
    make -j $(nproc) install && \
    make clean

# Copy the application source files into the image
WORKDIR /app/source
COPY tools/packaging/common/. ./tools/packaging/common/
COPY ./ArmoniK.SDK.Common ./ArmoniK.SDK.Common
COPY ./ArmoniK.SDK.Client ./ArmoniK.SDK.Client
COPY ./ArmoniK.SDK.Client.Test ./ArmoniK.SDK.Client.Test
COPY ./CMakeLists.txt ./
COPY ./Utils.cmake ./
COPY ./Packaging.cmake ./

# Build the application using the copied source files and protobuf definitions
WORKDIR /app/build
ARG CLIENT_VERSION
RUN cmake "-DCMAKE_INSTALL_PREFIX=/app/install" "-DINSTALL_SDK_DIR=/app/install" "-DBUILD_WORKER=OFF" "-DBUILD_DYNAMICWORKER=OFF" "-DBUILD_END2END=ON" -DVERSION="${CLIENT_VERSION}" /app/source/ && make -j $(nproc) install && make clean

FROM alpine:3.18.12 AS runner

RUN apk update && apk add --no-cache \
    c-ares \
    libprotobuf \
    grpc \
    grpc-cpp

ENV LD_LIBRARY_PATH="/app/install/lib"
ENV PATH="/app/install/bin:$PATH"

COPY --from=builder /armonik/api/. /app/install/.
COPY --from=builder /app/install/. /app/install/.

# Set the entrypoint for the application's test executable
# This is the command that will be executed when the container is run
ENTRYPOINT ["/app/install/bin/ArmoniK.SDK.Client.E2ETest"]
