FROM archlinux:latest

RUN pacman -Syu --noconfirm \
 && pacman -S --noconfirm --needed \
      cairo \
      gcc \
      git \
      jq \
      libxkbcommon \
      make \
      meson \
      ninja \
      niri \
      pkgconf \
      procps-ng \
      sway \
      wayland \
      wayland-protocols \
      wayland-utils \
      wtype \
      xdg-desktop-portal-gtk \
 && pacman -Scc --noconfirm

RUN useradd -m -u 1000 -s /bin/bash waynav
USER waynav
WORKDIR /src
