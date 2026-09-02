Data synchronisation tool for offensive cyber security assessment. Hand written code in incomplete state,
tried to explore the CDC algorithm but due to time constraints, the repo remains limited. Instead, checkout the branch
experimental/ai to see a working implementation of a sync based off the design document and diagram. 

![data_synchronisation_tool.drawio.png](docs/data_synchronisation_tool.drawio.png)

## Building

This is a CMake project (requires CMake >= 4.3 and a C23-capable compiler). Building from the
repo root generates both the `sync_client` and `sync_server` binaries:

```
cmake -B build -S .
cmake --build build
```

The resulting executables are placed at `build/client/sync_client` and `build/server/sync_server`.

