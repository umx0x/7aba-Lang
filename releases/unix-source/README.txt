7aba-Lang source package for Linux and macOS

This package contains the C99 interpreter source, a build script, and an
example program. It builds on the selected computer; it is not a prebuilt
Linux or macOS binary.

Requirements
------------
- A C99 compiler (GCC or Clang)
- make is not required

Build and run (Linux)
---------------------
  sh ./build.sh
  ./7aba run ./hello.7ba

Build and run (macOS)
---------------------
  sh ./build.sh
  ./7aba run ./hello.7ba

The interpreter is built in this folder. Put other .7ba files beside it
or pass their paths to the interpreter.
