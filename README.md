# luna

a simple game server

## build

MacOS:
```bash
# config
cmake -H. -Bbuild
# build
export MACOSX_DEPLOYMENT_TARGET="10.6"
cmake --build build
# test
ctest --test-dir build
```

### macos

```bash
brew install pkg-config
brew install autoconf
brew install cmake
```
