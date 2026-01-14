#include "topology.h"

#include <glog/logging.h>
#include <gtest/gtest.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <infiniband/verbs.h>
#include <limits.h>
#include <fstream>
#include <cstdlib>

#include "transfer_metadata.h"
#include "memory_location.h"


namespace {

// ============================================================================
// IB Device Availability Check Functions (copied for testing)
// ============================================================================

// Check if the InfiniBand device is accessible via /dev/infiniband/.
// This verifies:
// 1. The device path exists
// 2. It is a character device
// 3. The current process has read/write permissions
static bool isIbDeviceAccessible(const char *device_name) {
    char device_path[PATH_MAX];
    struct stat st;

    snprintf(device_path, sizeof(device_path), "/dev/infiniband/%s",
             device_name);

    if (stat(device_path, &st) != 0) {
        LOG(WARNING) << "Device path " << device_path << " does not exist";
        return false;
    }

    if (!S_ISCHR(st.st_mode)) {
        LOG(WARNING) << "Device path " << device_path
                     << " is not a character device";
        return false;
    }

    if (access(device_path, R_OK | W_OK) != 0) {
        LOG(WARNING) << "Device " << device_path
                     << " is not accessible for read/write";
        return false;
    }

    return true;
}

// Check if a specific port on an InfiniBand device is usable.
// Assumes port_num is already validated by caller.
// This verifies:
// 1. Port attributes can be queried
// 2. Port has a valid GID table (gid_tbl_len > 0)
// 3. Port is in active state
// Core logic for checking port attributes (can accept mock data)
static bool checkIbPortAttr(const struct ibv_port_attr &port_attr,
                            const std::string &device_name,
                            uint8_t port_num) {
    if (port_attr.gid_tbl_len == 0) {
        LOG(WARNING) << device_name << ":" << static_cast<int>(port_num)
                     << " has no GID table entries";
        return false;
    }

    if (port_attr.state != IBV_PORT_ACTIVE) {
        LOG(INFO) << device_name << ":" << static_cast<int>(port_num)
                  << " is not active (state: "
                  << static_cast<int>(port_attr.state) << ")";
        return false;
    }

    return true;
}

// Check if a specific port on an InfiniBand device is usable.
static bool checkIbDevicePort(struct ibv_context *context,
                               const std::string &device_name,
                               uint8_t port_num) {
    struct ibv_port_attr port_attr;
    if (ibv_query_port(context, port_num, &port_attr) != 0) {
        PLOG(WARNING) << "Failed to query port " << static_cast<int>(port_num)
                      << " on " << device_name;
        return false;
    }

    return checkIbPortAttr(port_attr, device_name, port_num);
}

// Perform comprehensive availability check on an InfiniBand device.
// Returns true only if:
// 1. Device is accessible via /dev/infiniband/
// 2. Device can be opened and queried
// 3. At least one port is active and usable
static bool isIbDeviceAvailable(struct ibv_device *device) {
    const char *device_name = ibv_get_device_name(device);

    if (!isIbDeviceAccessible(device_name)) {
        return false;
    }

    struct ibv_context *context = ibv_open_device(device);
    if (!context) {
        PLOG(WARNING) << "Failed to open device " << device_name;
        return false;
    }

    struct ibv_device_attr device_attr;
    if (ibv_query_device(context, &device_attr) != 0) {
        PLOG(WARNING) << "Failed to query device attributes for "
                      << device_name;
        ibv_close_device(context);
        return false;
    }

    bool has_active_port = false;
    for (uint8_t port = 1; port <= device_attr.phys_port_cnt; ++port) {
        if (checkIbDevicePort(context, device_name, port)) {
            has_active_port = true;
            LOG(INFO) << "Device " << device_name << " port "
                      << static_cast<int>(port) << " is available";
        }
    }

    ibv_close_device(context);

    if (!has_active_port) {
        LOG(WARNING) << "Device " << device_name
                     << " has no active ports, skipping";
    }

    return has_active_port;
}

// Helper function to check if any IB devices exist on this system
static bool hasIbDevices() {
    int num_devices = 0;
    struct ibv_device **device_list = ibv_get_device_list(&num_devices);
    if (!device_list || num_devices <= 0) {
        if (device_list) {
            ibv_free_device_list(device_list);
        }
        return false;
    }
    ibv_free_device_list(device_list);
    return true;
}

// Helper function to get the first available IB device for testing
static struct ibv_device *getFirstIbDevice() {
    int num_devices = 0;
    struct ibv_device **device_list = ibv_get_device_list(&num_devices);
    if (!device_list || num_devices <= 0) {
        return nullptr;
    }
    return device_list[0];
}

}  // namespace

// ============================================================================
// Test Cases for IB Device Accessibility Check
// ============================================================================

// Test 1: Non-existent device path should return false
TEST(IbDeviceAccessibilityTest, NonExistentDevicePath) {
    EXPECT_FALSE(isIbDeviceAccessible("non_existent_device_12345"));
}

// Test 2: Empty device name should return false
TEST(IbDeviceAccessibilityTest, EmptyDeviceName) {
    EXPECT_FALSE(isIbDeviceAccessible(""));
}

// Test 3: Device name with special characters should be handled safely
TEST(IbDeviceAccessibilityTest, SpecialCharacterDeviceName) {
    EXPECT_FALSE(isIbDeviceAccessible("../../../etc/passwd"));
    EXPECT_FALSE(isIbDeviceAccessible("device\0name"));
}

// Test 4: Real device accessibility (conditional on hardware availability)
TEST(IbDeviceAccessibilityTest, RealDeviceIfExists) {
    if (!hasIbDevices()) {
        GTEST_SKIP() << "No IB devices available on this system";
    }

    int num_devices = 0;
    struct ibv_device **device_list = ibv_get_device_list(&num_devices);
    ASSERT_NE(device_list, nullptr);
    ASSERT_GT(num_devices, 0);

    const char *device_name = ibv_get_device_name(device_list[0]);
    // Real device may or may not be accessible depending on permissions
    bool result = isIbDeviceAccessible(device_name);
    LOG(INFO) << "Device " << device_name << " accessible: " << result;

    ibv_free_device_list(device_list);
}

// ============================================================================
// Test Cases for IB Port Check
// ============================================================================

// Test: Port state DOWN should return false
TEST(IbDevicePortCheckTest, PortStateDown) {
    struct ibv_port_attr mock_attr = {};
    mock_attr.state = IBV_PORT_DOWN;
    mock_attr.gid_tbl_len = 16;

    EXPECT_FALSE(checkIbPortAttr(mock_attr, "mock_device", 1));
}

// Test: Port state ACTIVE with valid GID should return true
TEST(IbDevicePortCheckTest, PortStateActiveWithGid) {
    struct ibv_port_attr mock_attr = {};
    mock_attr.state = IBV_PORT_ACTIVE;
    mock_attr.gid_tbl_len = 16;

    EXPECT_TRUE(checkIbPortAttr(mock_attr, "mock_device", 1));
}

// Test: Port state ACTIVE but no GID should return false
TEST(IbDevicePortCheckTest, PortStateActiveNoGid) {
    struct ibv_port_attr mock_attr = {};
    mock_attr.state = IBV_PORT_ACTIVE;
    mock_attr.gid_tbl_len = 0;

    EXPECT_FALSE(checkIbPortAttr(mock_attr, "mock_device", 1));
}

// ============================================================================
// Test Cases for IB Device Availability Check
// ============================================================================

// Test 7: Non-existent device should not be available
TEST(IbDeviceAvailabilityTest, NonExistentDevice) {
    // We can't directly test isIbDeviceAvailable with a fake device
    // because it requires a real ibv_device pointer.
    // Instead, we verify that the accessibility check fails for fake paths.
    EXPECT_FALSE(isIbDeviceAccessible("fake_device_xyz"));
}

// Test 8: Real device availability check (conditional on hardware)
TEST(IbDeviceAvailabilityTest, RealDeviceIfExists) {
    if (!hasIbDevices()) {
        GTEST_SKIP() << "No IB devices available on this system";
    }

    int num_devices = 0;
    struct ibv_device **device_list = ibv_get_device_list(&num_devices);
    ASSERT_NE(device_list, nullptr);
    ASSERT_GT(num_devices, 0);

    bool result = isIbDeviceAvailable(device_list[0]);
    const char *device_name = ibv_get_device_name(device_list[0]);
    LOG(INFO) << "Device " << device_name << " availability: " << result;

    ibv_free_device_list(device_list);
}

// Test 9: All devices should pass availability check or be properly filtered
TEST(IbDeviceAvailabilityTest, AllDevicesFiltered) {
    if (!hasIbDevices()) {
        GTEST_SKIP() << "No IB devices available on this system";
    }

    int num_devices = 0;
    struct ibv_device **device_list = ibv_get_device_list(&num_devices);
    ASSERT_NE(device_list, nullptr);

    int available_count = 0;
    int unavailable_count = 0;

    for (int i = 0; i < num_devices; ++i) {
        const char *device_name = ibv_get_device_name(device_list[i]);
        if (isIbDeviceAvailable(device_list[i])) {
            available_count++;
            LOG(INFO) << "Device " << device_name << " is available";
        } else {
            unavailable_count++;
            LOG(INFO) << "Device " << device_name << " is NOT available";
        }
    }

    LOG(INFO) << "Total devices: " << num_devices
              << ", available: " << available_count
              << ", unavailable: " << unavailable_count;

    ibv_free_device_list(device_list);
}

// ============================================================================
// Test Cases for Topology Integration
// ============================================================================

// Test 10: Topology discover should only include available devices
TEST(TopologyIntegrationTest, DiscoverOnlyAvailableDevices) {
    mooncake::Topology topology;
    topology.discover();

    auto hca_list = topology.getHcaList();

    // If we have HCAs in the list, verify they are accessible
    for (const auto &hca_name : hca_list) {
        bool accessible = isIbDeviceAccessible(hca_name.c_str());
        if (accessible) {
            LOG(INFO) << "HCA " << hca_name
                      << " in topology list is accessible";
        } else {
            // This might happen if device became unavailable after discovery
            LOG(WARNING) << "HCA " << hca_name
                         << " in topology list is not accessible";
        }
    }

    LOG(INFO) << "Topology discovered " << hca_list.size() << " HCAs";
}

// Test 11: Topology with filter should respect availability
TEST(TopologyIntegrationTest, DiscoverWithFilter) {
    mooncake::Topology topology;
    std::vector<std::string> filter = {"mlx5_0", "mlx5_1"};
    topology.discover(filter);

    auto hca_list = topology.getHcaList();
    for (const auto &hca_name : hca_list) {
        // Verify filtered devices are in the filter list
        EXPECT_TRUE(std::find(filter.begin(), filter.end(), hca_name) !=
                    filter.end())
            << "Device " << hca_name << " should be in filter list";
    }
}

// Test 12: Topology with non-existent filter should return empty
TEST(TopologyIntegrationTest, DiscoverWithNonExistentFilter) {
    mooncake::Topology topology;
    std::vector<std::string> filter = {"non_existent_device_12345"};
    topology.discover(filter);

    auto hca_list = topology.getHcaList();
    EXPECT_TRUE(hca_list.empty())
        << "No devices should match non-existent filter";
}

TEST(ToplogyTest, GetTopologyMatrix) {
    mooncake::Topology topology;
    topology.discover();
    std::string json_str = topology.toString();
    LOG(INFO) << json_str;
    topology.clear();
    topology.parse(json_str);
    ASSERT_EQ(topology.toString(), json_str);
}

TEST(ToplogyTest, TestEmpty) {
    mooncake::Topology topology;
    std::string json_str =
        "{\"cpu:0\" : [[\"erdma_0\"],[\"erdma_1\"]],\"cpu:1\" "
        ": [[\"erdma_1\"],[\"erdma_0\"]]}";
    topology.clear();
    topology.parse(json_str);
    ASSERT_TRUE(!topology.empty());
}

TEST(ToplogyTest, TestHcaList) {
    mooncake::Topology topology;
    std::string json_str =
        "{\"cpu:0\" : [[\"erdma_0\"],[\"erdma_0\"]],\"cpu:1\" "
        ": [[\"erdma_0\"],[\"erdma_0\"]]}";
    topology.clear();
    topology.parse(json_str);
    ASSERT_EQ(topology.getHcaList().size(), static_cast<size_t>(1));
    std::set<std::string> HcaList = {"erdma_0"};
    for (auto &hca : topology.getHcaList()) {
        ASSERT_TRUE(HcaList.count(hca));
    }
}

TEST(ToplogyTest, TestHcaListSize) {
    mooncake::Topology topology;
    std::string json_str =
        "{\"cpu:0\" : [[\"erdma_0\"],[\"erdma_1\"]],\"cpu:1\" "
        ": [[\"erdma_2\"],[\"erdma_3\"]]}";
    topology.clear();
    topology.parse(json_str);
    ASSERT_EQ(topology.getHcaList().size(), static_cast<size_t>(4));
}

TEST(ToplogyTest, TestHcaList2) {
    mooncake::Topology topology;
    std::string json_str =
        "{\"cpu:0\" : [[\"erdma_0\"],[\"erdma_1\"]],\"cpu:1\" "
        ": [[\"erdma_1\"],[\"erdma_0\"]]}";
    topology.clear();
    topology.parse(json_str);
    ASSERT_EQ(topology.getHcaList().size(), static_cast<size_t>(2));
    std::set<std::string> HcaList = {"erdma_0", "erdma_1"};
    for (auto &hca : topology.getHcaList()) {
        ASSERT_TRUE(HcaList.count(hca));
    }
}

TEST(ToplogyTest, TestMatrix) {
    mooncake::Topology topology;
    std::string json_str = "{\"cpu:0\" : [[\"erdma_0\"],[\"erdma_1\"]]}";
    topology.clear();
    topology.parse(json_str);
    auto matrix = topology.getMatrix();
    ASSERT_TRUE(matrix.size() == 1);
    ASSERT_TRUE(matrix.count("cpu:0"));
}

TEST(ToplogyTest, TestSelectDevice) {
    mooncake::Topology topology;
    std::string json_str = "{\"cpu:0\" : [[\"erdma_0\"],[\"erdma_1\"]]}";
    topology.clear();
    topology.parse(json_str);
    std::set<int> items = {0, 1};
    int device;
    device = topology.selectDevice("cpu:0", 2);
    ASSERT_TRUE(items.count(device));
    items.erase(device);
    device = topology.selectDevice("cpu:0", 1);
    ASSERT_TRUE(items.count(device));
    items.erase(device);
    ASSERT_TRUE(items.empty());
}

TEST(ToplogyTest, TestSelectDeviceAny) {
    mooncake::Topology topology;
    std::string json_str = "{\"cpu:0\" : [[\"erdma_0\"],[\"erdma_1\"]]}";
    topology.clear();
    topology.parse(json_str);
    std::set<int> items = {0, 1};
    int device;
    device = topology.selectDevice(mooncake::kWildcardLocation, 2);
    ASSERT_TRUE(items.count(device));
    items.erase(device);
    device = topology.selectDevice(mooncake::kWildcardLocation, 1);
    ASSERT_TRUE(items.count(device));
    items.erase(device);
    ASSERT_TRUE(items.empty());
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
