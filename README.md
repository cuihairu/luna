# luna

a simple game server

## build

MacOS:
```bash
# config
cmake -H. -Bbuild -DCMAKE_BUILD_TYPE=Debug
# build
export MACOSX_DEPLOYMENT_TARGET="10.6"
cmake --build build
# test
ctest --test-dir build
```
Windows:
```bash
# config
cmake -H. -Bbuild
# build
cmake --build build --config "Debug"
# test
ctest --test-dir build --config "Debug"
```
Linux:
```bash
# config
cmake -H. -Bbuild -DCMAKE_BUILD_TYPE=Debug
# build
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
