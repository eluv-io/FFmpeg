ARG UF_BASE_IMAGE=uf_render_interface:ubuntu_22
FROM ${UF_BASE_IMAGE}

ENV DEBIAN_FRONTEND=noninteractive
ENV LD_LIBRARY_PATH=/runtime/lib:/runtime/lib/uf:/runtime/lib/3rdparty

RUN apt-get update && \
    apt-get install --no-install-recommends --yes \
    build-essential \
    ca-certificates \
    nasm \
    perl \
    pkg-config \
    python3 \
    yasm && \
    apt-get clean && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /workspace/FFmpeg

CMD ["bash"]