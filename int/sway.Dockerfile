FROM debian:bookworm-slim

RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      ca-certificates \
      build-essential \
      git \
      meson \
      ninja-build \
      pkg-config \
      python3-pip \
      libwayland-dev \
      wayland-protocols \
      libxkbcommon-dev \
      libcairo2-dev \
      sway \
      swaybg \
      wayland-utils \
      procps \
      jq \
      wtype \
 && python3 -m pip install --break-system-packages 'meson>=1.1,<2' \
 && rm -rf /var/lib/apt/lists/* /root/.cache/pip

RUN useradd -m -u 1000 -s /bin/bash waynav
USER waynav
WORKDIR /src
