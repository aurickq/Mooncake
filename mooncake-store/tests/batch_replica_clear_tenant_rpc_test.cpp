#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <ylt/coro_rpc/coro_rpc_server.hpp>

#include "common/network.h"
#include "master_client.h"
#include "master_config.h"
#include "rpc_service.h"
#include "tenant/quota_policy_store.h"
#include "types.h"

namespace mooncake {
namespace {

class ScopedPolicyFile {
   public:
    ScopedPolicyFile() {
        const UUID suffix = generate_uuid();
        path_ = std::filesystem::temp_directory_path() /
                ("batch_replica_clear_tenants_" + std::to_string(suffix.first) +
                 "_" + std::to_string(suffix.second) + ".yaml");
    }

    ~ScopedPolicyFile() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }

    const std::filesystem::path& path() const { return path_; }

   private:
    std::filesystem::path path_;
};

class BatchReplicaClearTenantRpcTest : public ::testing::TestWithParam<bool> {};

TEST_P(BatchReplicaClearTenantRpcTest,
       UsesClientTenantAndKeepsTenantsIsolated) {
    constexpr char kTenant[] = "tenant-a";
    constexpr char kSharedKey[] = "shared-batch-clear-key";
    constexpr uint64_t kObjectSize = 1024;

    ScopedPolicyFile policy_file;
    TenantQuotaPolicySnapshot policy;
    policy.tenant_quotas = {
        {std::string(TenantId::kDefaultValue), 64 * 1024 * 1024},
        {kTenant, 64 * 1024 * 1024},
    };
    if (GetParam()) {
        std::ofstream output(policy_file.path());
        ASSERT_TRUE(output.is_open());
        output << FormatTenantQuotaPolicyYaml(policy);
        ASSERT_TRUE(output.good());
    }

    WrappedMasterServiceConfig service_config;
    service_config.default_kv_lease_ttl = 0;
    service_config.enable_metric_reporting = false;
    service_config.enable_multi_tenants = true;
    if (GetParam()) {
        service_config.tenant_quota_connector_type = "file";
        service_config.tenant_quota_connector_uri = policy_file.path().string();
    }
    auto service = std::make_shared<WrappedMasterService>(service_config);

    const auto ports = getFreeTcpPorts(1);
    ASSERT_EQ(ports.size(), 1u);
    coro_rpc::coro_rpc_server server(
        /*thread_num=*/2, /*port=*/ports.front(), /*address=*/"127.0.0.1",
        std::chrono::seconds(0), /*tcp_no_delay=*/true);
    RegisterRpcService(server, *service);
    auto start_result = server.async_start();
    ASSERT_FALSE(start_result.hasResult());
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    const UUID client_id = generate_uuid();
    Segment segment;
    segment.id = generate_uuid();
    segment.name = "batch_replica_clear_tenant_segment";
    segment.base = 0x300000000;
    segment.size = 16 * 1024 * 1024;
    ASSERT_TRUE(service->MountSegment(segment, client_id).has_value());

    ReplicateConfig replicate_config;
    replicate_config.replica_num = 1;
    const std::vector<std::string> keys = {kSharedKey, "batch-key", "regex-key",
                                           "all-key"};
    for (const std::string& tenant :
         {std::string(kTenant), std::string(TenantId::kDefaultValue)}) {
        MasterClient client(client_id, nullptr, tenant);
        ASSERT_EQ(client.Connect("127.0.0.1:" + std::to_string(ports.front())),
                  ErrorCode::OK);
        const size_t size = tenant == kTenant ? kObjectSize : 2 * kObjectSize;
        for (const auto& key : keys) {
            auto put = client.PutStart(key, {size}, replicate_config);
            ASSERT_TRUE(put.has_value()) << toString(put.error());
            ASSERT_TRUE(client.PutEnd({key, std::nullopt}, ReplicaType::MEMORY)
                            .has_value());
        }
        auto read = client.GetReplicaList(kSharedKey);
        ASSERT_TRUE(read.has_value());
        ASSERT_EQ(read->replicas.size(), 1u);
        EXPECT_EQ(read->replicas.front()
                      .get_memory_descriptor()
                      .buffer_descriptor.size_,
                  size);
    }

    {
        MasterClient tenant_client(client_id, nullptr, kTenant);
        ASSERT_EQ(
            tenant_client.Connect("127.0.0.1:" + std::to_string(ports.front())),
            ErrorCode::OK);
        auto cleared =
            tenant_client.BatchReplicaClear({kSharedKey}, client_id, "");
        ASSERT_TRUE(cleared.has_value()) << toString(cleared.error());
        EXPECT_EQ(cleared.value(), std::vector<std::string>{kSharedKey});
        auto batch = tenant_client.BatchRemove({"batch-key"}, true);
        ASSERT_EQ(batch.size(), 1u);
        ASSERT_TRUE(batch.front().has_value());
        auto regex = tenant_client.RemoveByRegex("^regex-key$", true);
        ASSERT_TRUE(regex.has_value());
        EXPECT_EQ(regex.value(), 1);
        auto all = tenant_client.RemoveAll(true);
        ASSERT_TRUE(all.has_value());
        EXPECT_EQ(all.value(), 1);
    }

    auto tenant_exists = service->ExistKey(kSharedKey, kTenant);
    ASSERT_TRUE(tenant_exists.has_value());
    EXPECT_FALSE(tenant_exists.value());
    auto default_exists =
        service->ExistKey(kSharedKey, std::string(TenantId::kDefaultValue));
    ASSERT_TRUE(default_exists.has_value());
    EXPECT_TRUE(default_exists.value());

    {
        MasterClient default_client(client_id);
        ASSERT_EQ(default_client.Connect("127.0.0.1:" +
                                         std::to_string(ports.front())),
                  ErrorCode::OK);
        auto present = default_client.BatchExistKey(keys);
        ASSERT_EQ(present.size(), keys.size());
        for (const auto& result : present) {
            ASSERT_TRUE(result.has_value());
            EXPECT_TRUE(result.value());
        }
        auto cleared = default_client.RemoveAll(true);
        ASSERT_TRUE(cleared.has_value());
        EXPECT_EQ(cleared.value(), static_cast<long>(keys.size()));
    }

    default_exists =
        service->ExistKey(kSharedKey, std::string(TenantId::kDefaultValue));
    ASSERT_TRUE(default_exists.has_value());
    EXPECT_FALSE(default_exists.value());

    // An omitted identity remains the default namespace; malformed identities
    // must not silently become the default when quota enforcement is absent.
    auto empty = service->PutStart(client_id, "empty", kObjectSize,
                                   replicate_config, "");
    EXPECT_EQ(empty.has_value(), !GetParam());
    if (!GetParam()) {
        ASSERT_TRUE(service
                        ->PutEnd(client_id, {"empty", std::nullopt},
                                 ReplicaType::MEMORY, "")
                        .has_value());
        EXPECT_TRUE(service->ExistKey("empty").value());
        EXPECT_FALSE(service->ExistKey("empty", kTenant).value());
        auto quotas = service->ListTenantQuotaSnapshots();
        ASSERT_FALSE(quotas.has_value());
        EXPECT_EQ(quotas.error(), ErrorCode::UNAVAILABLE_IN_CURRENT_MODE);
    }
    EXPECT_FALSE(service
                     ->PutStart(client_id, "invalid", kObjectSize,
                                replicate_config, "_invalid")
                     .has_value());
    EXPECT_FALSE(service->ExistKey("empty", "_invalid").has_value());

    server.stop();
}

INSTANTIATE_TEST_SUITE_P(QuotaModes, BatchReplicaClearTenantRpcTest,
                         ::testing::Bool(),
                         [](const ::testing::TestParamInfo<bool>& info) {
                             return info.param ? "WithPolicy" : "WithoutPolicy";
                         });

}  // namespace
}  // namespace mooncake
