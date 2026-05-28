# src/contrib

Place third-party source code referenced directly by `src/vkfwd` here.

Keep each dependency in its own subdirectory and include license files with the
vendored source. Prefer system packages or generated code outside this directory
when the dependency does not need to be compiled into the forwarding layer.

Current shallow submodules:

- `fmt`: fmtlib/fmt `12.1.0`
- `spdlog`: gabime/spdlog `v1.17.0`

Initialize with:

```sh
git submodule update --init --depth 1 src/contrib/fmt src/contrib/spdlog
```
