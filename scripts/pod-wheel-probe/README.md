# Isolated Mooncake wheel probe

This probes GPU RDMA WRITE and READ between two existing pods without changing
their serving packages or restarting their workloads. Run from a machine with
`kubectl` access to both pods. Each container needs `timeout`, `tar`, and an
existing Python environment with a compatible PyTorch installation.

The wheel is unpacked using Python's standard library, preserving the package
and its sibling auditwheel libraries. No package installer is needed. This
probe expects the module and libraries at the wheel root, not in `.data` paths;
it does not install command-line entry points.

```bash
bash run.sh /path/to/package.whl CONTEXT NAMESPACE SOURCE_POD TARGET_POD CONTAINER /path/to/python
```

The defaults are GPU 0, NIC `mlx5_2`, and GID index 5. Verify these match the
nodes before starting; override with `GPU`, `NIC`, and `GID_INDEX` if necessary.
The GPU needs at least 1 GiB free after CUDA context initialization. Explicit
payload storage is 1 MiB per process, plus the CUDA context and allocator cache.

The runner creates separate temporary wheel installations and starts fresh
processes for each condition:

- `MC_RDMA_DATA_DIRECT=1`: PCIe DMA-BUF export and mlx5 Data Direct registration.
- `MC_RDMA_DATA_DIRECT=0`: ordinary CUDA DMA-BUF export and registration.

Both conditions disable NVIDIA peermem and expandable CUDA allocations only in
the probe processes. The target checks bytes written into its GPU buffer and
changes the payload; the initiator then reads and checks the new bytes.

Both endpoints must complete successfully for a condition to pass. The runner
returns nonzero if either condition fails, including the ordinary-path control.
A Data Direct pass with an ordinary-path failure is a meaningful result, not a
reason to discard the successful condition. Preserve both logs for diagnosis.

Temporary directories remain for inspection. No deployment changes, driver
changes, or serving-process environment changes are performed. Success verifies
the wheel and one selected GPU/NIC path, not full model serving or all NICs.
