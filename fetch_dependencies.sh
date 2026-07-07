#!/bin/bash
# Downloads the prebuilt dependency xcframeworks (folium-app/SharedDependencies release 2.0)
# into Binaries/ and the boost 1.85.0 headers into Dependencies/boost.
# Both directories are git-ignored — run once after cloning. See BUILDING.md.
set -euo pipefail
cd "$(dirname "$0")"

RELEASE="https://github.com/folium-app/SharedDependencies/releases/download/2.0"
LIBS=(
  libboostcontext libboostiostreams libboostprogramoptions libboostserialization
  lib_cubeb lib_dynarmic lib_enet lib_faad2 lib_fmt lib_genericcodegen lib_glslang
  lib_machineindependent lib_mcl lib_openal lib_opus lib_sdl3 lib_sirit
  lib_soundtouch lib_spirv lib_teakra
)

mkdir -p Binaries
for lib in "${LIBS[@]}"; do
  name="${lib}.xcframework"
  inner="${lib//_/}"   # zips unpack to underscore-less names (lib_fmt.zip → libfmt.xcframework)
  if [ -d "Binaries/${name}" ] || [ -d "Binaries/${inner}.xcframework" ] || [ -d "Binaries/SDL3.xcframework" -a "$lib" = "lib_sdl3" ]; then
    echo "✓ ${name} (cached)"
    continue
  fi
  echo "↓ ${name}"
  curl -sfL -o "Binaries/${name}.zip" "${RELEASE}/${name}.zip"
  unzip -q -o "Binaries/${name}.zip" -d Binaries
  rm -rf "Binaries/${name}.zip" Binaries/__MACOSX
done

# libglslang's Headers are the glslang dir CONTENT (Include/, MachineIndependent/, …) but
# its own SPIRV headers include via the "glslang/…" prefix — nest them one level down.
for slice in Binaries/libglslang.xcframework/*/Headers; do
  if [ -d "$slice/Include" ] && [ ! -d "$slice/glslang" ]; then
    mkdir "$slice/glslang"
    mv "$slice"/{Include,MachineIndependent,GenericCodeGen,OSDependent,Public,HLSL} \
       "$slice/glslang/" 2>/dev/null || true
    echo "✚ nested glslang/ prefix in ${slice}"
  fi
done

if [ ! -d Dependencies/vulkan-headers/include ]; then
  echo "↓ Vulkan-Headers v1.4.304"
  git clone --quiet --depth 1 --branch v1.4.304 \
    https://github.com/KhronosGroup/Vulkan-Headers.git Dependencies/vulkan-headers
else
  echo "✓ Vulkan-Headers (cached)"
fi

if [ ! -d Binaries/MoltenVK.xcframework ]; then
  echo "↓ MoltenVK v1.4.1 (runtime Vulkan ICD — allocate() dlopens @rpath/MoltenVK.framework)"
  curl -sfL -o /tmp/MoltenVK-ios.tar \
    "https://github.com/KhronosGroup/MoltenVK/releases/download/v1.4.1/MoltenVK-ios.tar"
  tar -xf /tmp/MoltenVK-ios.tar -C /tmp MoltenVK/MoltenVK/dynamic/MoltenVK.xcframework
  mv /tmp/MoltenVK/MoltenVK/dynamic/MoltenVK.xcframework Binaries/
  rm -rf /tmp/MoltenVK-ios.tar /tmp/MoltenVK
else
  echo "✓ MoltenVK (cached)"
fi

if [ ! -f Dependencies/vma/vma/vk_mem_alloc.h ]; then
  echo "↓ VulkanMemoryAllocator v3.1.0 (header-only)"
  mkdir -p Dependencies/vma/vma
  curl -sfL -o Dependencies/vma/vma/vk_mem_alloc.h \
    "https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/v3.1.0/include/vk_mem_alloc.h"
else
  echo "✓ VulkanMemoryAllocator (cached)"
fi

if [ ! -f Dependencies/spirv-headers/spirv/unified1/spirv.hpp11 ]; then
  echo "↓ SPIRV-Headers spirv.hpp11 (sirit.h includes it; not in libsirit's Headers)"
  mkdir -p Dependencies/spirv-headers/spirv/unified1
  curl -sfL -o Dependencies/spirv-headers/spirv/unified1/spirv.hpp11 \
    "https://raw.githubusercontent.com/KhronosGroup/SPIRV-Headers/vulkan-sdk-1.4.304.0/include/spirv/unified1/spirv.hpp11"
else
  echo "✓ SPIRV-Headers (cached)"
fi

if [ ! -d Dependencies/zstd/lib ]; then
  echo "↓ zstd 1.5.6 source (lib + seekable_format contrib)"
  mkdir -p Dependencies/zstd
  curl -sfL -o /tmp/zstd-1.5.6.tar.gz \
    "https://github.com/facebook/zstd/releases/download/v1.5.6/zstd-1.5.6.tar.gz"
  tar -xzf /tmp/zstd-1.5.6.tar.gz -C Dependencies/zstd --strip-components=1 \
    zstd-1.5.6/lib zstd-1.5.6/contrib/seekable_format
  rm -f /tmp/zstd-1.5.6.tar.gz
  # The seekable contrib includes zstd's internal "mem.h" bare; point it at lib/common
  # rather than adding that dir to the global search path (its compiler.h/debug.h names
  # are shadow-prone).
  sed -i '' 's|#include "mem.h"|#include "../../lib/common/mem.h"|' \
    Dependencies/zstd/contrib/seekable_format/zstdseek_compress.c \
    Dependencies/zstd/contrib/seekable_format/zstdseek_decompress.c
else
  echo "✓ zstd (cached)"
fi

if [ ! -d Dependencies/boost/boost ]; then
  echo "↓ boost 1.85.0 headers (~140MB download)"
  mkdir -p Dependencies/boost
  curl -sfL -o /tmp/boost_1_85_0.tar.gz \
    "https://archives.boost.io/release/1.85.0/source/boost_1_85_0.tar.gz"
  tar -xzf /tmp/boost_1_85_0.tar.gz -C Dependencies/boost --strip-components=1 boost_1_85_0/boost
  rm -f /tmp/boost_1_85_0.tar.gz
else
  echo "✓ boost headers (cached)"
fi

echo "Done. Binaries/: $(ls Binaries | wc -l | tr -d ' ') xcframeworks; boost: $(ls Dependencies/boost/boost | wc -l | tr -d ' ') entries"
