// Copyright 2017 The Ray Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "ray/gcs/gcs_client/global_state_accessor.h"

#include <memory>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "ray/common/asio/instrumented_io_context.h"
#include "ray/gcs/gcs_server/gcs_server.h"
#include "ray/gcs/test/gcs_test_util.h"
#include "ray/rpc/gcs_server/gcs_rpc_client.h"
#include "ray/util/path_utils.h"

namespace ray {

class GlobalStateAccessorTest : public ::testing::TestWithParam<bool> {
 public:
  GlobalStateAccessorTest() {
    if (GetParam()) {
      RayConfig::instance().gcs_storage() = "memory";
    } else {
      RayConfig::instance().gcs_storage() = "redis";
    }

    if (!GetParam()) {
      TestSetupUtil::StartUpRedisServers(std::vector<int>());
    }
  }

  virtual ~GlobalStateAccessorTest() {
    if (!GetParam()) {
      TestSetupUtil::ShutDownRedisServers();
    }
  }

 protected:
  void SetUp() override {
    RayConfig::instance().gcs_max_active_rpcs_per_handler() = -1;

    config.grpc_server_port = 6379;

    config.node_ip_address = "127.0.0.1";
    config.grpc_server_name = "MockedGcsServer";
    config.grpc_server_thread_num = 1;

    if (!GetParam()) {
      config.redis_address = "127.0.0.1";
      config.enable_sharding_conn = false;
      config.redis_port = TEST_REDIS_SERVER_PORTS.front();
    }
    io_service_.reset(new instrumented_io_context());
    gcs_server_.reset(new gcs::GcsServer(config, *io_service_));
    gcs_server_->Start();
    work_ = std::make_unique<
        boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>(
        io_service_->get_executor());
    thread_io_service_.reset(new std::thread([this] { io_service_->run(); }));

    // Wait until server starts listening.
    while (!gcs_server_->IsStarted()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Create GCS client and global state.
    gcs::GcsClientOptions options("127.0.0.1",
                                  6379,
                                  ClusterID::Nil(),
                                  /*allow_cluster_id_nil=*/true,
                                  /*fetch_cluster_id_if_nil=*/false);
    gcs_client_ = std::make_unique<gcs::GcsClient>(options);
    global_state_ = std::make_unique<gcs::GlobalStateAccessor>(options);
    RAY_CHECK_OK(gcs_client_->Connect(*io_service_));

    RAY_CHECK(global_state_->Connect());
  }

  void TearDown() override {
    // Make sure any pending work with pointers to gcs_server_ is not run after
    // gcs_server_ is destroyed.
    io_service_->stop();

    global_state_->Disconnect();
    global_state_.reset();

    gcs_client_->Disconnect();
    gcs_client_.reset();

    gcs_server_->Stop();
    if (!GetParam()) {
      TestSetupUtil::FlushAllRedisServers();
    }

    thread_io_service_->join();
    gcs_server_.reset();
  }

  // GCS server.
  gcs::GcsServerConfig config;
  std::unique_ptr<gcs::GcsServer> gcs_server_;
  std::unique_ptr<std::thread> thread_io_service_;
  std::unique_ptr<instrumented_io_context> io_service_;

  // GCS client.
  std::unique_ptr<gcs::GcsClient> gcs_client_;

  std::unique_ptr<gcs::GlobalStateAccessor> global_state_;

  // Timeout waiting for GCS server reply, default is 2s.
  const std::chrono::milliseconds timeout_ms_{2000};
  std::unique_ptr<
      boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>
      work_;
};

TEST_P(GlobalStateAccessorTest, TestJobTable) {
  int job_count = 100;
  ASSERT_EQ(global_state_->GetAllJobInfo().size(), 0);
  for (int index = 0; index < job_count; ++index) {
    auto job_id = JobID::FromInt(index);
    auto job_table_data = Mocker::GenJobTableData(job_id);
    std::promise<bool> promise;
    gcs_client_->Jobs().AsyncAdd(
        job_table_data, [&promise](Status status) { promise.set_value(status.ok()); });
    promise.get_future().get();
  }
  ASSERT_EQ(global_state_->GetAllJobInfo().size(), job_count);
}

// Test GetAllJobInfo where some jobs were submitted by the Ray Job API (i.d. they have
// job_submission_id set).
TEST_P(GlobalStateAccessorTest, TestJobTableWithSubmissionId) {
  int job_count = 100;
  ASSERT_EQ(global_state_->GetAllJobInfo().size(), 0);
  for (int index = 0; index < job_count; ++index) {
    auto job_id = JobID::FromInt(index);
    auto job_table_data = Mocker::GenJobTableData(job_id);
    if (index % 2 == 0) {
      (*job_table_data->mutable_config()->mutable_metadata())["job_submission_id"] =
          std::to_string(index);
    }
    std::promise<bool> promise;
    gcs_client_->Jobs().AsyncAdd(
        job_table_data, [&promise](Status status) { promise.set_value(status.ok()); });
    promise.get_future().get();
  }
  ASSERT_EQ(global_state_->GetAllJobInfo().size(), job_count);
}

TEST_P(GlobalStateAccessorTest, TestNodeTable) {
  int node_count = 100;
  ASSERT_EQ(global_state_->GetAllNodeInfo().size(), 0);
  // It's useful to check if index value will be marked as address suffix.
  for (int index = 0; index < node_count; ++index) {
    auto node_table_data =
        Mocker::GenNodeInfo(index,
                            std::string("127.0.0.") + std::to_string(index),
                            "Mocker_node_" + std::to_string(index * 10));
    std::promise<bool> promise;
    gcs_client_->Nodes().AsyncRegister(
        *node_table_data, [&promise](Status status) { promise.set_value(status.ok()); });
    WaitReady(promise.get_future(), timeout_ms_);
  }
  auto node_table = global_state_->GetAllNodeInfo();
  ASSERT_EQ(node_table.size(), node_count);
  for (int index = 0; index < node_count; ++index) {
    rpc::GcsNodeInfo node_data;
    node_data.ParseFromString(node_table[index]);
    ASSERT_EQ(node_data.node_manager_address(),
              std::string("127.0.0.") + std::to_string(node_data.node_manager_port()));
    ASSERT_EQ(
        node_data.node_name(),
        std::string("Mocker_node_") + std::to_string(node_data.node_manager_port() * 10));
  }
}

TEST_P(GlobalStateAccessorTest, TestGetAllTotalResources) {
  ASSERT_EQ(global_state_->GetAllTotalResources().size(), 0);

  // Register node
  auto node_table_data = Mocker::GenNodeInfo();
  node_table_data->mutable_resources_total()->insert({"CPU", 1});
  node_table_data->mutable_resources_total()->insert({"GPU", 10});

  std::promise<bool> promise;
  gcs_client_->Nodes().AsyncRegister(
      *node_table_data, [&promise](Status status) { promise.set_value(status.ok()); });
  WaitReady(promise.get_future(), timeout_ms_);
  ASSERT_EQ(global_state_->GetAllNodeInfo().size(), 1);

  // Assert get total resources right.
  std::vector<rpc::TotalResources> all_total_resources;
  for (const auto &string_of_total_resources_by_node_id :
       global_state_->GetAllTotalResources()) {
    rpc::TotalResources total_resources_by_node_id;
    total_resources_by_node_id.ParseFromString(string_of_total_resources_by_node_id);
    all_total_resources.push_back(total_resources_by_node_id);
  }
  ASSERT_EQ(all_total_resources.size(), 1);
  ASSERT_EQ(all_total_resources[0].resources_total_size(), 2);
  ASSERT_EQ((*all_total_resources[0].mutable_resources_total())["CPU"], 1.0);
  ASSERT_EQ((*all_total_resources[0].mutable_resources_total())["GPU"], 10.0);
}

TEST_P(GlobalStateAccessorTest, TestGetAllResourceUsage) {
  std::unique_ptr<std::string> resources = global_state_->GetAllResourceUsage();
  rpc::ResourceUsageBatchData resource_usage_batch_data;
  resource_usage_batch_data.ParseFromString(*resources.get());
  ASSERT_EQ(resource_usage_batch_data.batch_size(), 0);

  auto node_table_data = Mocker::GenNodeInfo();
  node_table_data->mutable_resources_total()->insert({"CPU", 1});

  std::promise<bool> promise;
  gcs_client_->Nodes().AsyncRegister(
      *node_table_data, [&promise](Status status) { promise.set_value(status.ok()); });
  WaitReady(promise.get_future(), timeout_ms_);
  auto node_table = global_state_->GetAllNodeInfo();
  ASSERT_EQ(node_table.size(), 1);

  // Report resource usage first time.
  std::promise<bool> promise1;
  syncer::ResourceViewSyncMessage resources1;
  gcs_server_->UpdateGcsResourceManagerInTest(
      NodeID::FromBinary(node_table_data->node_id()), resources1);

  resources = global_state_->GetAllResourceUsage();
  resource_usage_batch_data.ParseFromString(*resources.get());
  ASSERT_EQ(resource_usage_batch_data.batch_size(), 1);

  // Report changed resource usage.
  std::promise<bool> promise2;
  syncer::ResourceViewSyncMessage resources2;
  (*resources2.mutable_resources_total())["CPU"] = 1;
  (*resources2.mutable_resources_total())["GPU"] = 10;
  (*resources2.mutable_resources_available())["CPU"] = 1;
  (*resources2.mutable_resources_available())["GPU"] = 5;
  gcs_server_->UpdateGcsResourceManagerInTest(
      NodeID::FromBinary(node_table_data->node_id()), resources2);

  resources = global_state_->GetAllResourceUsage();
  resource_usage_batch_data.ParseFromString(*resources.get());
  ASSERT_EQ(resource_usage_batch_data.batch_size(), 1);
  auto resources_data = resource_usage_batch_data.mutable_batch()->at(0);
  ASSERT_EQ(resources_data.resources_total_size(), 2);
  ASSERT_EQ((*resources_data.mutable_resources_total())["CPU"], 1.0);
  ASSERT_EQ((*resources_data.mutable_resources_total())["GPU"], 10.0);
  ASSERT_EQ(resources_data.resources_available_size(), 2);
  ASSERT_EQ((*resources_data.mutable_resources_available())["CPU"], 1.0);
  ASSERT_EQ((*resources_data.mutable_resources_available())["GPU"], 5.0);
}

TEST_P(GlobalStateAccessorTest, TestWorkerTable) {
  ASSERT_EQ(global_state_->GetAllWorkerInfo().size(), 0);
  // Add worker info
  auto worker_table_data = Mocker::GenWorkerTableData();
  worker_table_data->mutable_worker_address()->set_worker_id(
      WorkerID::FromRandom().Binary());
  ASSERT_TRUE(global_state_->AddWorkerInfo(worker_table_data->SerializeAsString()));

  // Get worker info
  auto worker_id = WorkerID::FromBinary(worker_table_data->worker_address().worker_id());
  ASSERT_TRUE(global_state_->GetWorkerInfo(worker_id));

  // Add another worker info
  auto another_worker_data = Mocker::GenWorkerTableData();
  another_worker_data->mutable_worker_address()->set_worker_id(
      WorkerID::FromRandom().Binary());
  ASSERT_TRUE(global_state_->AddWorkerInfo(another_worker_data->SerializeAsString()));
  ASSERT_EQ(global_state_->GetAllWorkerInfo().size(), 2);
}

TEST_P(GlobalStateAccessorTest, TestUpdateWorkerDebuggerPort) {
  ASSERT_EQ(global_state_->GetAllWorkerInfo().size(), 0);
  // Add worker info
  auto worker_table_data = Mocker::GenWorkerTableData();
  worker_table_data->mutable_worker_address()->set_worker_id(
      WorkerID::FromRandom().Binary());
  ASSERT_TRUE(global_state_->AddWorkerInfo(worker_table_data->SerializeAsString()));

  // Get worker info
  auto worker_id = WorkerID::FromBinary(worker_table_data->worker_address().worker_id());
  ASSERT_TRUE(global_state_->GetWorkerInfo(worker_id));

  // Update the worker debugger port
  auto debugger_port = 10000;
  ASSERT_TRUE(global_state_->UpdateWorkerDebuggerPort(worker_id, debugger_port));

  // Verify the debugger port
  auto another_worker_table_data = Mocker::GenWorkerTableData();
  auto worker_info = global_state_->GetWorkerInfo(worker_id);
  ASSERT_TRUE(another_worker_table_data->ParseFromString(*worker_info));
  ASSERT_EQ(another_worker_table_data->debugger_port(), debugger_port);
}

TEST_P(GlobalStateAccessorTest, TestUpdateWorkerNumPausedThreads) {
  ASSERT_EQ(global_state_->GetAllWorkerInfo().size(), 0);
  // Add worker info
  auto worker_table_data = Mocker::GenWorkerTableData();
  worker_table_data->mutable_worker_address()->set_worker_id(
      WorkerID::FromRandom().Binary());
  ASSERT_TRUE(global_state_->AddWorkerInfo(worker_table_data->SerializeAsString()));

  // Get worker info
  auto worker_id = WorkerID::FromBinary(worker_table_data->worker_address().worker_id());
  ASSERT_TRUE(global_state_->GetWorkerInfo(worker_id));

  // Update the worker num paused threads
  auto num_paused_threads_delta = 2;
  ASSERT_TRUE(
      global_state_->UpdateWorkerNumPausedThreads(worker_id, num_paused_threads_delta));

  // Verify the num paused threads is equal to num_paused_threads_delta
  auto another_worker_table_data = Mocker::GenWorkerTableData();
  auto worker_info = global_state_->GetWorkerInfo(worker_id);
  ASSERT_TRUE(another_worker_table_data->ParseFromString(*worker_info));
  ASSERT_EQ(another_worker_table_data->num_paused_threads(), num_paused_threads_delta);

  // Update the worker num paused threads again and verify it is equal to 0
  ASSERT_TRUE(
      global_state_->UpdateWorkerNumPausedThreads(worker_id, -num_paused_threads_delta));
  worker_info = global_state_->GetWorkerInfo(worker_id);
  ASSERT_TRUE(another_worker_table_data->ParseFromString(*worker_info));
  ASSERT_EQ(another_worker_table_data->num_paused_threads(), 0);
}

// TODO(sang): Add tests after adding asyncAdd
TEST_P(GlobalStateAccessorTest, TestPlacementGroupTable) {
  ASSERT_EQ(global_state_->GetAllPlacementGroupInfo().size(), 0);
}

INSTANTIATE_TEST_SUITE_P(RedisRemovalTest,
                         GlobalStateAccessorTest,
                         ::testing::Values(false, true));

}  // namespace ray

int main(int argc, char **argv) {
  ray::RayLog::InstallFailureSignalHandler(argv[0]);
  InitShutdownRAII ray_log_shutdown_raii(
      ray::RayLog::StartRayLog,
      ray::RayLog::ShutDownRayLog,
      argv[0],
      ray::RayLogLevel::INFO,
      ray::GetLogFilepathFromDirectory(/*log_dir=*/"", /*app_name=*/argv[0]),
      ray::GetErrLogFilepathFromDirectory(/*log_dir=*/"", /*app_name=*/argv[0]),
      ray::RayLog::GetRayLogRotationMaxBytesOrDefault(),
      ray::RayLog::GetRayLogRotationBackupCountOrDefault());
  ::testing::InitGoogleTest(&argc, argv);
  RAY_CHECK(argc == 3);
  ray::TEST_REDIS_SERVER_EXEC_PATH = argv[1];
  ray::TEST_REDIS_CLIENT_EXEC_PATH = argv[2];
  return RUN_ALL_TESTS();
}
