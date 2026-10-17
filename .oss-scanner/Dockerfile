# Builds the game-free parts of F4Multiplayer on Linux for Anthropic's OSS Scanner: the protocol
# and relay server (common/), the dedicated server (server/), the test bot (bot/) and the unit
# tests (tests/), then runs the tests. The F4SE plugin itself (src/**.cpp) needs Fallout 4 and
# MSVC, so it is not built here; the game-free headers it shares with the tests are.
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential g++-14 git curl ca-certificates unzip \
    && rm -rf /var/lib/apt/lists/* \
    && update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-14 100 \
    && update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-14 100

# xmake, the project's build tool (pinned).
RUN curl -fsSL https://xmake.io/shget.text | bash -s v2.9.9 \
    && ln -s /root/.local/bin/xmake /usr/local/bin/xmake
ENV XMAKE_ROOT=y

WORKDIR /src
COPY . /src

# The game-free targets; enet is fetched by xmake during this (online) step.
RUN xmake f -y -m releasedbg -p linux --toolchain=gcc --portable=y \
    && xmake build -y F4MPCommon F4MPServer F4MPBot F4MPTests

# The tests must pass in the image: that is what the scanner starts from.
RUN ./build/linux/x86_64/releasedbg/F4MPTests

CMD ["/bin/bash"]
