#!/usr/bin/env bash
# Same pinned LLVM signing authority and compiler family as the existing Linux CI gate.
set -euo pipefail
sudo apt-get update
sudo apt-get install --yes ca-certificates cmake curl gnupg ninja-build \
  libfontconfig1-dev libva-dev python3-dev patchelf make pkg-config
bloom_llvm_key=$(mktemp)
trap 'rm -f "$bloom_llvm_key"' EXIT
curl --fail --silent --show-error --location https://apt.llvm.org/llvm-snapshot.gpg.key \
  --output "$bloom_llvm_key"
bloom_llvm_fingerprint=$(gpg --show-keys --with-colons "$bloom_llvm_key" | awk -F: '$1 == "fpr" {print $10; exit}')
test "$bloom_llvm_fingerprint" = 6084F3CF814B57C1CF12EFD515CF4D18AF4F7421
gpg --dearmor < "$bloom_llvm_key" | sudo tee /usr/share/keyrings/llvm-snapshot.gpg >/dev/null
printf '%s\n' 'deb [signed-by=/usr/share/keyrings/llvm-snapshot.gpg] https://apt.llvm.org/noble/ llvm-toolchain-noble-22 main' \
  | sudo tee /etc/apt/sources.list.d/llvm-22.list >/dev/null
sudo apt-get update
sudo apt-get install --yes clang-22
