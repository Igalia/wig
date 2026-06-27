FROM fedora:44

RUN dnf install -y dnf-plugins-core && \
    dnf copr enable -y philn/wpewebkit && \
    dnf install -y \
      clang \
      clang-tools-extra \
      git \
      meson \
      wpewebkit-devel \
      gtk4-devel \
      libepoxy-devel \
      libadwaita-devel \
      flex \
      bison && \
    dnf clean all
