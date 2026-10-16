# Multi-Tenant Deployment

## Configure the Master

Tenant namespaces and quota admission are disabled by default:

| Master configuration | Behavior |
|----------------------|----------|
| `enable_multi_tenants=false` | Client tenant IDs are ignored; objects use the `default` namespace |
| `enable_multi_tenants=true`, both quota connector fields empty | Separate tenant namespaces sharing physical capacity, without tenant registration or quotas |
| `enable_multi_tenants=true`, quota connector configured | Separate tenant namespaces with strict tenant registration and memory quota admission |

### Namespace Isolation Without Quotas

To isolate object keys without assigning storage budgets, omit both quota connector settings:

```bash
mooncake_master --enable_multi_tenants=true
```

Clients with different valid `tenant_id` values use separate object namespaces, even when their object keys are identical. No tenant policy file or registration is required. Tenants share the physical storage pool and its existing global eviction policy; there are no per-tenant capacity reservations or eviction guarantees, and a busy tenant can displace another tenant's cache entries. Quota accounting, quota-driven eviction, and the quota admin API are disabled.

Tenant IDs are supplied by clients and are not authenticated. Use this feature with trusted clients; it does not prevent a client from selecting another tenant's ID.

### File Connector

Configure a connector when you want memory writes admitted against connector-managed per-tenant quotas:

```bash
mooncake_master \
  --enable_multi_tenants=true \
  --tenant_quota_connector_type=file \
  --tenant_quota_connector_uri=/etc/mooncake/tenant_quotas.yaml
```

If `tenant_quota_connector_uri` is set and `tenant_quota_connector_type` is omitted or empty, the connector defaults to `file`, preserving URI-only configurations. An explicit `file` or `etcd` type requires a non-empty URI. Invalid connector settings or an unreadable policy fail startup; they never fall back to namespace-only mode.

### etcd Connector

You can also store the same YAML policy in etcd when Mooncake Store is built with `STORE_USE_ETCD=ON`:

```bash
mooncake_master \
  --enable_multi_tenants=true \
  --cluster_id=mooncake_cluster \
  --tenant_quota_connector_type=etcd \
  --tenant_quota_connector_uri=127.0.0.1:2379
```

The etcd connector stores the policy at `mooncake-store/<cluster_id>/tenant_quota_policy`. If the key does not exist, the master starts with an empty policy so the first tenant policy can be created through the admin API. It shares the process-wide store etcd client used by HA/oplog, so if HA or oplog also uses etcd, `tenant_quota_connector_uri` must match those etcd endpoints.

## Define the Tenant Policy

This section applies when a quota connector is configured. The policy must use schema version `1`; tenant names must be non-empty, unique, must not start with `_`, and must not contain NUL or control characters; quotas must be positive integers with optional `B`, `KB`, `MB`, `GB`, or `TB` units:

```yaml
version: 1

tenants:
  - name: tenant-a
    quota: 200GB

  - name: tenant-b
    quota: 500GB
```

When strict multi-tenant mode is enabled, write requests must include a registered tenant. The `default` tenant is not special unless it is explicitly registered in the connector policy.

## Manage Tenant Quotas

The same HTTP port used for metrics exposes the tenant quota admin API:

```bash
# List tenant quota snapshots
curl -s http://<master_host>:9003/api/v1/tenant_quotas

# Query one tenant
curl -s "http://<master_host>:9003/api/v1/tenant_quotas?tenant_id=tenant-a"

# Upsert an explicit policy. Explicit tenant policies must be between 1 byte
# and 2^63 - 1 bytes.
curl -s -X PUT "http://<master_host>:9003/api/v1/tenant_quotas?tenant_id=tenant-a" \
  -H 'Content-Type: application/json' \
  -d '{"requested_quota_bytes":2147483648}'

# Delete an explicit policy. The tenant must not own objects or quota usage.
curl -s -X DELETE "http://<master_host>:9003/api/v1/tenant_quotas?tenant_id=tenant-a"
```

Each tenant quota snapshot returns:

```json
{
  "success": true,
  "data": {
    "tenant_id": "tenant-a",
    "requested_quota_bytes": 2147483648,
    "effective_quota_bytes": 2147483648,
    "charged_bytes": 0,
    "admission_closed": false,
    "over_quota": false,
    "has_explicit_policy": true
  }
}
```

`charged_bytes` includes completed MEMORY replicas and in-flight MEMORY allocations. Put, Copy, Move, and promotion charge quota when admission starts; failed, revoked, partially completed, or expired operations refund the unused charge. `admission_closed` is `true` when the account rejects new writes, including after its explicit policy is removed.

In HA mode, quota admin requests are served only by the active master service. Standby, candidate, or inactive services return HTTP 503. Without quota admission, including namespace-only mode, the quota admin API returns HTTP 409 with `UNAVAILABLE_IN_CURRENT_MODE`. Deleting a non-empty tenant returns HTTP 409 with `TENANT_NOT_EMPTY`.

## SGLang

When Mooncake is used as the HiCache storage backend, set `tenant_id` in the
Mooncake backend configuration:

```bash
--hicache-storage-backend mooncake \
--hicache-storage-backend-extra-config \
  '{"master_server_address":"127.0.0.1:50051","tenant_id":"tenant-a"}'
```

Alternatively, add `tenant_id` to the JSON file selected by
`SGLANG_HICACHE_MOONCAKE_CONFIG_PATH`, or use `MOONCAKE_TENANT_ID` when loading
the Mooncake configuration from environment variables. SGLang forwards the
resolved value to the Mooncake client.

All prefill, decode, and replica instances that should share KV cache entries
must use the same `tenant_id` and compatible model and release namespaces.

## vLLM

Add `tenant_id` to the Mooncake client JSON configuration:

```json
"tenant_id": "tenant-a"
```

Point `MOONCAKE_CONFIG_PATH` at that file and enable
`MooncakeStoreConnector` through `--kv-transfer-config`:

```bash
MOONCAKE_CONFIG_PATH=/path/to/mooncake_config.json \
vllm serve <model> \
  --kv-transfer-config \
  '{"kv_connector":"MooncakeStoreConnector","kv_role":"kv_both"}'
```

`MooncakeStoreConnector` reads the JSON during initialization and passes its
tenant ID to the Mooncake client.

All prefill, decode, and replica instances that should share KV cache entries
must use the same `tenant_id` and compatible model and release namespaces.
