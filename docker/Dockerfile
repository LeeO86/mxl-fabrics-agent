# syntax=docker/dockerfile:1

FROM node:22-bookworm AS webui
WORKDIR /web
COPY web/package.json web/package-lock.json* ./
RUN npm install --no-fund --no-audit
COPY web/ ./
RUN npm run build

FROM ubuntu:24.04 AS build
ARG MXL_REF=218ddaa0a08c12ffe75fc475ae65aa3d9eef16d7
ARG LIBFABRIC_REF=v2.3.1
ARG NMOS_CPP_REF=fe303849527394b03bdedc8f161f377fe458bb62

RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build git pkg-config curl zip unzip tar ca-certificates \
        bison flex autoconf automake libtool \
        libibverbs-dev librdmacm-dev rdma-core \
        libboost-system-dev libboost-thread-dev libboost-chrono-dev \
        libboost-date-time-dev libboost-regex-dev libboost-random-dev \
        libboost-filesystem-dev libcpprest-dev nlohmann-json3-dev \
        libssl-dev libavahi-compat-libdnssd-dev \
    && rm -rf /var/lib/apt/lists/*

RUN curl -fsSL -o /tmp/libfabric.tar.bz2 "https://github.com/ofiwg/libfabric/releases/download/${LIBFABRIC_REF}/libfabric-${LIBFABRIC_REF#v}.tar.bz2" \
    && tar -xf /tmp/libfabric.tar.bz2 -C /tmp \
    && cd "/tmp/libfabric-${LIBFABRIC_REF#v}" \
    && ./configure --prefix=/usr/local --enable-verbs=yes --enable-tcp=yes --enable-shm=yes \
        --disable-usnic --disable-efa --disable-opx --disable-psm3 --disable-rxm \
    && make -j"$(nproc)" \
    && make install \
    && ldconfig

RUN git clone --depth 1 https://github.com/microsoft/vcpkg /opt/vcpkg \
    && /opt/vcpkg/bootstrap-vcpkg.sh -disableMetrics
ENV VCPKG_ROOT=/opt/vcpkg PKG_CONFIG_PATH=/usr/local/lib/pkgconfig

RUN git init /tmp/mxl \
    && git -C /tmp/mxl remote add origin https://github.com/dmf-mxl/mxl.git \
    && git -C /tmp/mxl fetch --depth 1 origin "${MXL_REF}" \
    && git -C /tmp/mxl checkout FETCH_HEAD
COPY docker/mxl-vcpkg.json /tmp/mxl-manifest/vcpkg.json
# The build image has no Python. Insert the vcpkg baseline with sed.
RUN BASELINE="$(git -C /opt/vcpkg rev-parse HEAD)" \
    && sed -i "1a\\  \"builtin-baseline\": \"${BASELINE}\"," /tmp/mxl-manifest/vcpkg.json
RUN cmake -S /tmp/mxl -B /tmp/mxl/build -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE=/opt/vcpkg/scripts/buildsystems/vcpkg.cmake \
        -DVCPKG_MANIFEST_DIR=/tmp/mxl-manifest \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_TESTS=OFF -DBUILD_TOOLS=OFF -DBUILD_UTILS=OFF -DBUILD_DOCS=OFF \
        -DMXL_ENABLE_FABRICS_OFI=ON \
        -DCMAKE_INSTALL_PREFIX=/opt/mxl \
    && cmake --build /tmp/mxl/build -j"$(nproc)" \
    && cmake --install /tmp/mxl/build

RUN git init /opt/nmos-cpp \
    && git -C /opt/nmos-cpp remote add origin https://github.com/sony/nmos-cpp.git \
    && git -C /opt/nmos-cpp fetch --depth 1 origin "${NMOS_CPP_REF}" \
    && git -C /opt/nmos-cpp checkout FETCH_HEAD

COPY . /src
COPY --from=webui /web/dist /src/web/dist
RUN cmake -S /src -B /src/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_PREFIX_PATH="/opt/mxl;/tmp/mxl/build/vcpkg_installed/x64-linux" \
        -DMFA_WITH_NMOS=ON -DNMOS_CPP_DIR=/opt/nmos-cpp/Development \
    && cmake --build /src/build -j"$(nproc)" \
    && /src/build/unit-tests

FROM ubuntu:24.04 AS runtime
# ibverbs-providers holds the user-space NIC drivers (irdma for E810, mlx5, rxe) and
# /etc/libibverbs.d; without it libibverbs finds no device and verbs has no provider.
# ibverbs-utils: ibv_devices and ibv_devinfo, to check what the container sees.
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        ca-certificates curl \
        libibverbs1 librdmacm1 rdma-core ibverbs-providers ibverbs-utils \
        libcpprest2.10 libboost-chrono1.83.0t64 libboost-thread1.83.0 \
        libssl3t64 libavahi-compat-libdnssd1 \
    && rm -rf /var/lib/apt/lists/*
COPY --from=build /usr/local/lib/libfabric* /usr/local/lib/
COPY --from=build /opt/mxl/lib/ /usr/local/lib/
COPY --from=build /src/build/mxl-fabrics-agent /usr/local/bin/mxl-fabrics-agent
COPY --from=build /src/build/mxl-pattern-writer /usr/local/bin/mxl-pattern-writer
COPY tests/integration/fake_nmos.py /usr/local/share/mxl-fabrics-agent/fake_nmos.py
# Ubuntu 24.04 already has uid 1000 (user ubuntu). useradd --uid 1000 then exits 4.
RUN ldconfig \
    && if ! getent passwd 1000 >/dev/null; then \
         useradd --uid 1000 --no-create-home --shell /usr/sbin/nologin mxl; \
       fi
USER 1000:1000
LABEL org.opencontainers.image.source="https://github.com/LeeO86/mxl-fabrics-agent" \
      org.opencontainers.image.licenses="MIT" \
      io.dmf.mxl.revision="218ddaa0a08c12ffe75fc475ae65aa3d9eef16d7"
EXPOSE 8095 3232 3233
ENTRYPOINT ["/usr/local/bin/mxl-fabrics-agent"]
