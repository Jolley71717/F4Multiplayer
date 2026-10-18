# Builds the game-free parts of F4Multiplayer on Linux for Anthropic's OSS Scanner: the protocol
# and relay server (common/), the dedicated server (server/), the test bot (bot/) and the unit
# tests (tests/), then runs the tests. The F4SE plugin itself (src/**.cpp) needs Fallout 4 and
# MSVC, so xmake.lua leaves its target out on Linux; the game-free headers it shares with the
# tests are built through them.
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive
# gcc 14 (C++23: <format>, <print>); build-essential's own gcc is 13, so g++/gcc are pointed at 14.
# git, make and a C compiler are what xmake's installer builds xmake with; unzip is for xmake's
# package fetches.
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential gcc-14 g++-14 git curl ca-certificates unzip \
    && rm -rf /var/lib/apt/lists/* \
    && update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-14 100 \
    && update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-14 100

# xmake, the project's build tool, pinned to the version the project is built with; its official
# installer builds that tag from source into /root/.local/bin.
RUN curl -fsSL https://xmake.io/shget.text | bash -s v3.1.2 \
    && ln -s /root/.local/bin/xmake /usr/local/bin/xmake
ENV XMAKE_ROOT=y

WORKDIR /src
COPY . /src

# The game-free targets; enet (and cmake and ninja to build it) are fetched by xmake during this
# (online) step.
RUN xmake f -y -m releasedbg -p linux --toolchain=gcc \
    && xmake build -y F4MPCommon F4MPServer F4MPBot F4MPTests

# The tests must pass in the image: that is what the scanner starts from. The exit code is the
# number of failed tests; a known bug still failing is not one.
RUN ./build/linux/x86_64/releasedbg/F4MPTests

CMD ["/bin/bash"]
