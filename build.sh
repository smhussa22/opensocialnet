set -e

cmake -S . -B build \
  -DCMAKE_TOOLCHAIN_FILE=/home/faraz/vcpkg/scripts/buildsystems/vcpkg.cmake

cmake --build build