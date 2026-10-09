BUILD_DIR="build"

if [[ -d "$BUILD_DIR" ]]; then
    cmake --build build -j $(nproc)
else
    cmake -S . -B build
    cmake --build build -j $(nproc)
fi

./build/ChaTerm