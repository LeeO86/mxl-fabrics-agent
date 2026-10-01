# mxl-fabrics-agent

Per-host agent that replicates MXL flows between hosts with the MXL 1.1 Fabrics API.
`SPECIFICATION.md` is the contract. `IMPLEMENTATION_PLAN.md` records how the code follows it.

## Build

The image build in `docker/Dockerfile` is the supported path. A local build needs:

- CMake ≥ 3.24, Ninja, GCC ≥ 12
- libfabric ≥ 2.3 with the `tcp` and `verbs` providers (`pkg-config`)
- MXL `218ddaa0a08c12ffe75fc475ae65aa3d9eef16d7` built with `-DMXL_ENABLE_FABRICS_OFI=ON`
- Sony nmos-cpp at `fe303849527394b03bdedc8f161f377fe458bb62` when `-DMFA_WITH_NMOS=ON`

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_PREFIX_PATH=/opt/mxl \
  -DMFA_WITH_NMOS=ON \
  -DNMOS_CPP_DIR=/opt/nmos-cpp/Development
cmake --build build
./build/unit-tests
LD_LIBRARY_PATH=/opt/mxl/lib:/usr/local/lib FI_PROVIDER=tcp tests/integration/tcp_mesh.sh
```

`MXL_ROOT` must exist or the process exits 78. For a loopback run, a directory on `/dev/shm` is enough.

## Runtime notes

- Default web port is 8095. NMOS is 3232 and 3233. Fabric target ports start at 23500.
- `verbs` covers both Intel E810 (`irdma`) and NVIDIA ConnectX (`mlx5`).
- DeckLink and the ST 2110 gateway need `MIRROR_MODE=eager` because they reject an unknown `mxl_domain_id` at IS-05 activation.
