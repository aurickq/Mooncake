#!/usr/bin/env python3
"""Bounded wheel-only GPU WRITE/READ check, adapted from the wheel transfer test."""

import argparse
import json
import os
from pathlib import Path
import socket


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("role", choices=("target", "initiator"))
    parser.add_argument("--local-ip", required=True)
    parser.add_argument("--peer-ip", required=True)
    parser.add_argument("--control-port", type=int, default=0)
    parser.add_argument("--gpu", type=int, required=True)
    parser.add_argument("--nic", required=True)
    parser.add_argument("--gid-index", type=int, default=5)
    parser.add_argument("--wheel-root", type=Path, required=True)
    parser.add_argument("--data-direct", choices=("0", "1"), required=True)
    args = parser.parse_args()
    assert 0 <= args.control_port <= 65535
    assert args.role == "target" or args.control_port != 0
    assert args.gpu >= 0
    assert 0 <= args.gid_index <= 255
    assert "," not in args.nic and args.nic.startswith("mlx5_")

    os.environ.update(
        MC_RDMA_DATA_DIRECT=args.data_direct,
        WITH_NVIDIA_PEERMEM="0",
        MC_FORCE_HCA="1",
        MC_GID_INDEX=str(args.gid_index),
        MC_WORKERS_PER_CTX="1",
        MC_NUM_QP_PER_EP="1",
        MC_RETRY_CNT="1",
        MC_TRANSFER_TIMEOUT="10",
        MC_HANDSHAKE_CONNECT_TIMEOUT="5",
        MC_RPC_CLIENT_IO_THREADS="1",
        MC_TE_RPC_CLIENT_IO_THREADS="1",
        PYTORCH_ALLOC_CONF="expandable_segments:False",
        PYTORCH_CUDA_ALLOC_CONF="expandable_segments:False",
        OMP_NUM_THREADS="1",
    )
    for key in (
        "MC_USE_TENT",
        "MC_USE_TEV1",
        "MC_CUSTOM_TOPO_JSON",
        "MC_LEGACY_RPC_PORT_BINDING",
        "MC_FORCE_TCP",
        "MC_FORCE_SHM",
        "MC_FORCE_MNNVL",
        "MC_INTRANODE_NVLINK",
        "MC_USE_IPV6",
    ):
        os.environ.pop(key, None)

    import mooncake.engine as mooncake_engine
    import torch

    module_path = Path(mooncake_engine.__file__).resolve()
    assert module_path.is_relative_to(args.wheel_root.resolve()), module_path
    assert mooncake_engine.SUPPORT_CUDA
    torch.set_num_threads(1)
    torch.cuda.set_device(args.gpu)
    free_bytes, total_bytes = torch.cuda.mem_get_info()
    if free_bytes < 1024**3:
        raise RuntimeError(
            f"Need at least 1 GiB free GPU memory before probe allocation; have {free_bytes} bytes"
        )
    nbytes = 1024 * 1024
    write_pattern = torch.frombuffer(
        bytearray(range(256)) * (nbytes // 256), dtype=torch.uint8
    )
    read_pattern = write_pattern.bitwise_xor(0x5A)
    zeros = torch.zeros(nbytes, dtype=torch.uint8)
    buffer = torch.empty(nbytes, dtype=torch.uint8, device=f"cuda:{args.gpu}")
    buffer.copy_(zeros)
    torch.cuda.synchronize()
    print(
        json.dumps(
            {
                "event": "wheel",
                "engine": str(module_path),
                "torch": torch.__version__,
                "gpu": args.gpu,
                "nic": args.nic,
                "gid_index": args.gid_index,
                "data_direct": args.data_direct,
                "payload_bytes": nbytes,
                "free_bytes_before_buffer": free_bytes,
                "total_gpu_bytes": total_bytes,
                "cuda_reserved_bytes": torch.cuda.memory_reserved(),
            }
        ),
        flush=True,
    )
    engine = mooncake_engine.TransferEngine()

    def check(status, operation):
        if status != 0:
            raise RuntimeError(f"{operation} failed: {status}")

    def send(stream, message):
        stream.write(json.dumps(message).encode() + b"\n")
        stream.flush()

    def receive(stream):
        line = stream.readline(4097)
        assert (
            line and len(line) <= 4096 and line.endswith(b"\n")
        ), "Invalid control message"
        return json.loads(line)

    check(
        engine.initialize(f"{args.local_ip}:0", "P2PHANDSHAKE", "rdma", args.nic),
        "initialize",
    )
    ptr = buffer.data_ptr()
    check(engine.register_memory(ptr, nbytes, f"cuda:{args.gpu}"), "register_memory")
    try:
        if args.role == "target":
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
                listener.settimeout(45)
                listener.bind((args.local_ip, args.control_port))
                listener.listen(1)
                print(
                    json.dumps(
                        {"event": "ready", "control_port": listener.getsockname()[1]}
                    ),
                    flush=True,
                )
                connection, address = listener.accept()
                assert address[0] == args.peer_ip, address
                with connection:
                    connection.settimeout(30)
                    with connection.makefile("rwb") as stream:
                        send(
                            stream,
                            {
                                "segment": f"{args.local_ip}:{engine.get_rpc_port()}",
                                "pointer": ptr,
                                "nbytes": nbytes,
                            },
                        )
                        assert receive(stream) == {"op": "verify_write"}
                        torch.cuda.synchronize()
                        assert torch.equal(
                            buffer.cpu(), write_pattern
                        ), "Target WRITE payload mismatch"
                        buffer.copy_(read_pattern)
                        torch.cuda.synchronize()
                        send(stream, {"write": "PASS", "read_source": "ready"})
                        assert receive(stream) == {"read": "PASS"}
                        send(stream, {"done": "PASS"})
        else:
            with socket.create_connection(
                (args.peer_ip, args.control_port),
                timeout=30,
                source_address=(args.local_ip, 0),
            ) as connection:
                with connection.makefile("rwb") as stream:
                    peer = receive(stream)
                    assert peer["nbytes"] == nbytes
                    assert peer["segment"].startswith(args.peer_ip + ":")
                    buffer.copy_(write_pattern)
                    torch.cuda.synchronize()
                    check(
                        engine.transfer_sync_write(
                            peer["segment"], ptr, peer["pointer"], nbytes, "rdma"
                        ),
                        "WRITE",
                    )
                    send(stream, {"op": "verify_write"})
                    assert receive(stream) == {"write": "PASS", "read_source": "ready"}
                    buffer.copy_(zeros)
                    torch.cuda.synchronize()
                    check(
                        engine.transfer_sync_read(
                            peer["segment"], ptr, peer["pointer"], nbytes, "rdma"
                        ),
                        "READ",
                    )
                    torch.cuda.synchronize()
                    assert torch.equal(
                        buffer.cpu(), read_pattern
                    ), "Initiator READ payload mismatch"
                    send(stream, {"read": "PASS"})
                    assert receive(stream) == {"done": "PASS"}
        print(
            json.dumps(
                {"event": "result", "role": args.role, "write": "PASS", "read": "PASS"}
            ),
            flush=True,
        )
    finally:
        check(engine.unregister_memory(ptr), "unregister_memory")
        del engine
    del buffer
    torch.cuda.empty_cache()


if __name__ == "__main__":
    main()
