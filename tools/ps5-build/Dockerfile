# Build host for the NexusAI PS5 title. PS5_Vulkan supports Arch as its build host.
FROM archlinux:latest
RUN pacman -Syu --noconfirm --needed base-devel clang llvm lld clang18 lld18 make python python-pip python-mako \
      python-markupsafe python-yaml python-packaging meson ninja bison flex git curl wget unzip zip tar xz \
      pkgconf cmake glslang spirv-tools shaderc vulkan-headers spirv-headers mesa libglvnd ccache socat \
 && pacman -Scc --noconfirm
WORKDIR /work
