#!/usr/bin/env bash
# 在干净的 ubuntu:26.04 容器里验证 SV-COMP 归档自包含（核验清单 G2）。
#
# 检查声明依赖环境：只装 fm-tools-entry.yml 的 required_ubuntu_packages
# 和 unzip；Clang/LLVM 与 APRON 使用归档随带的版本。
# 通过表示归档可在该依赖环境中完成冒烟测试。
#
# 前置：Docker Desktop（Windows）已启用当前 WSL2 distro 的集成；
#       或 WSL2 内已装 docker.io 并启动 dockerd。
#
# 用法：bash svcomp/docker-smoketest.sh [归档路径]   # 默认 ../ikos-conc.zip
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ZIP="${1:-$SCRIPT_DIR/../ikos-conc.zip}"
ZIP="$(cd "$(dirname "$ZIP")" && pwd)/$(basename "$ZIP")"
[ -f "$ZIP" ] || { echo "归档不存在: $ZIP" >&2; exit 1; }

# 与 fm-tools-entry.yml required_ubuntu_packages 完全一致（外加 unzip）。
docker run --rm \
  -v "$ZIP:/tmp/ikos-conc.zip:ro" \
  docker.io/ubuntu:26.04 \
  bash -c '
    set -e
    export DEBIAN_FRONTEND=noninteractive
    apt-get update
    apt-get install -y --no-install-recommends \
      unzip \
      libc6-dev libc6-dev-i386 lib32gcc-15-dev \
      libgmp10 libgmpxx4ldbl libmpfr6 libppl14 \
      libboost-filesystem1.83.0 libboost-thread1.83.0 libtbb12 \
      libsqlite3-0 libtinfo6 zlib1g libffi8 \
      libedit2 libbsd0 liblzma5 libmd0 \
      python3
    cd /tmp
    unzip -q ikos-conc.zip
    cd ikos-conc
    ./smoketest.sh
    echo "=== SMOKETEST PASSED (ubuntu:26.04 clean container) ==="
  '
