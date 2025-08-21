Building
===

## Linux

Requirements:
  - CMake >= 3.25.0
  - GCC
  - libudev1

Build:

```shell
mkdir build
cd build
cmake ..
make
```

Alternatively, use https://github.com/bkerler/mtkclient

## Cross-compile for Windows

Requirements:
  - Docker

Prepare (build image with tools, will take some time):

```shell
make prepare
```

Build:

```shell
make windows
```