### Building a Debugger

[Source from the '_Building a Debugger_' book from NoStarch Press](https://nostarch.com/building-a-debugger)

Configuring with `vcpkg` (assumes `vcpkg` installed locally and `VCPKG_ROOT` set.)

```bash
# configure
cmake --preset linux-debug
# build
cmake --build out/build/linux-debug
```

If encountering issues with `PTRACE_ATTACH`:

```bash
setcap CAP_SYS_PTRACE=+eip sdb
```