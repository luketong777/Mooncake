#include "topology.h"

#include <glog/logging.h>
#include <gtest/gtest.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <chrono>

#include "transfer_metadata.h"
#include "memory_location.h"

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

// ==================== PCIe拓扑辅助函数测试 ====================

namespace {

// 为测试封装实际实现的函数，使其更易于测试
size_t getCommonParentLength(const char *path1, const char *path2) {
    if (!path1 || !path2) {
        return 0;
    }

    size_t offset = 0;
    size_t parent_length = 0;

    do {
        if (((path1[offset] == '/') || (path1[offset] == '\0')) &&
            ((path2[offset] == '/') || (path2[offset] == '\0'))) {
            parent_length = offset;
        }
    } while ((path1[offset] == path2[offset]) && (path1[offset++] != '\0'));

    return parent_length;
}

std::string getCommonParent(const char *path1, const char *path2) {
    size_t parent_length = getCommonParentLength(path1, path2);
    return std::string(path1, parent_length);
}

bool isPciRootComplex(const char *path) {
    if (!path) {
        return false;
    }

    int count = -1;
    if (sscanf(path, "/sys/devices/pci%*x:%*x%n", &count) < 0) {
        return false;
    }
    return (count == static_cast<int>(strlen(path)));
}

bool isSamePcieRootComplex(const char *bus1, const char *bus2) {
    if (!bus1 || !bus2) {
        return false;
    }

    char buf[PATH_MAX];
    char resolved1[PATH_MAX];
    char resolved2[PATH_MAX];

    snprintf(buf, sizeof(buf), "/sys/bus/pci/devices/%s", bus1);
    if (realpath(buf, resolved1) == nullptr) {
        return false;
    }

    snprintf(buf, sizeof(buf), "/sys/bus/pci/devices/%s", bus2);
    if (realpath(buf, resolved2) == nullptr) {
        return false;
    }

    std::string common = getCommonParent(resolved1, resolved2);
    if (common.empty()) {
        return false;
    }

    while (!common.empty()) {
        if (isPciRootComplex(common.c_str())) {
            return true;
        }

        auto pos = common.find_last_of('/');
        if (pos == std::string::npos) {
            break;
        }
        common.resize(pos);
    }

    return false;
}

}  // namespace

// ==================== getCommonParent 测试 ====================

TEST(PcieTopologyTest, GetCommonParentIdenticalPaths) {
    const char *path = "/sys/devices/pci0000:17/0000:17:01.0";
    std::string common = getCommonParent(path, path);
    EXPECT_EQ(common, path);
}

TEST(PcieTopologyTest, GetCommonParentSameRootDifferentDevices) {
    const char *path1 = "/sys/devices/pci0000:17/0000:17:01.0/device1";
    const char *path2 = "/sys/devices/pci0000:17/0000:17:02.0/device2";
    std::string common = getCommonParent(path1, path2);
    EXPECT_EQ(common, "/sys/devices/pci0000:17");
}

TEST(PcieTopologyTest, GetCommonParentDifferentRoots) {
    const char *path1 = "/sys/devices/pci0000:17/0000:17:01.0";
    const char *path2 = "/sys/devices/pci0000:85/0000:85:01.0";
    std::string common = getCommonParent(path1, path2);
    EXPECT_EQ(common, "/sys/devices");
}

TEST(PcieTopologyTest, GetCommonParentNestedPaths) {
    const char *path1 = "/sys/devices/pci0000:17/0000:17:01.0/0000:18:00.0";
    const char *path2 = "/sys/devices/pci0000:17/0000:17:01.0/0000:18:01.0";
    std::string common = getCommonParent(path1, path2);
    EXPECT_EQ(common, "/sys/devices/pci0000:17/0000:17:01.0");
}

TEST(PcieTopologyTest, GetCommonParentNoCommonPath) {
    const char *path1 = "/sys/devices/pci0000:17";
    const char *path2 = "/dev/infiniband/mlx5_0";
    std::string common = getCommonParent(path1, path2);
    EXPECT_TRUE(common.empty());
}

TEST(PcieTopologyTest, GetCommonParentNullInputs) {
    size_t len1 = getCommonParentLength(nullptr, "/sys/devices");
    EXPECT_EQ(len1, 0UL);
    
    size_t len2 = getCommonParentLength("/sys/devices", nullptr);
    EXPECT_EQ(len2, 0UL);
    
    size_t len3 = getCommonParentLength(nullptr, nullptr);
    EXPECT_EQ(len3, 0UL);
}

TEST(PcieTopologyTest, GetCommonParentEmptyStrings) {
    std::string common = getCommonParent("", "");
    EXPECT_TRUE(common.empty());
}

TEST(PcieTopologyTest, GetCommonParentOneEmpty) {
    std::string common = getCommonParent("/sys/devices", "");
    EXPECT_TRUE(common.empty());
}

TEST(PcieTopologyTest, GetCommonParentRootOnly) {
    const char *path1 = "/sys";
    const char *path2 = "/sys/devices";
    std::string common = getCommonParent(path1, path2);
    EXPECT_EQ(common, "/sys");
}

TEST(PcieTopologyTest, GetCommonParentWithTrailingSlash) {
    const char *path1 = "/sys/devices/pci0000:17/";
    const char *path2 = "/sys/devices/pci0000:17/0000:17:01.0";
    std::string common = getCommonParent(path1, path2);
    EXPECT_EQ(common, "/sys/devices/pci0000:17");
}

TEST(PcieTopologyTest, GetCommonParentPartialMatch) {
    const char *path1 = "/sys/devices/pci0000:17abc";
    const char *path2 = "/sys/devices/pci0000:17def";
    std::string common = getCommonParent(path1, path2);
    EXPECT_EQ(common, "/sys/devices");
}

TEST(PcieTopologyTest, GetCommonParentMultiLevelNested) {
    const char *path1 = "/sys/devices/pci0000:17/0000:17:01.0/0000:18:00.0/0000:19:00.0";
    const char *path2 = "/sys/devices/pci0000:17/0000:17:01.0/0000:18:00.0/0000:19:01.0";
    std::string common = getCommonParent(path1, path2);
    EXPECT_EQ(common, "/sys/devices/pci0000:17/0000:17:01.0/0000:18:00.0");
}

// ==================== isPciRootComplex 测试 ====================

TEST(PcieTopologyTest, IsPciRootComplexValid) {
    EXPECT_TRUE(isPciRootComplex("/sys/devices/pci0000:17"));
    EXPECT_TRUE(isPciRootComplex("/sys/devices/pci0000:85"));
    EXPECT_TRUE(isPciRootComplex("/sys/devices/pci0000:00"));
    EXPECT_TRUE(isPciRootComplex("/sys/devices/pci0000:ff"));
    EXPECT_TRUE(isPciRootComplex("/sys/devices/pci0001:00"));
}

TEST(PcieTopologyTest, IsPciRootComplexInvalid) {
    EXPECT_FALSE(isPciRootComplex("/sys/devices/pci0000:17/0000:17:01.0"));
    EXPECT_FALSE(isPciRootComplex("/sys/devices/pci0000:17/device"));
    EXPECT_FALSE(isPciRootComplex("/sys/devices"));
    EXPECT_FALSE(isPciRootComplex("/dev/pci0000:17"));
}

TEST(PcieTopologyTest, IsPciRootComplexEdgeCases) {
    EXPECT_FALSE(isPciRootComplex(nullptr));
    EXPECT_FALSE(isPciRootComplex(""));
    EXPECT_FALSE(isPciRootComplex("pci0000:17"));
    EXPECT_FALSE(isPciRootComplex("/sys/devices/pci"));
    EXPECT_FALSE(isPciRootComplex("/sys/devices/pci0000:17/"));
}

TEST(PcieTopologyTest, IsPciRootComplexDifferentFormats) {
    EXPECT_TRUE(isPciRootComplex("/sys/devices/pci000a:0b"));
    EXPECT_TRUE(isPciRootComplex("/sys/devices/pci00ff:ff"));
    EXPECT_FALSE(isPciRootComplex("/sys/devices/pci:17"));
    EXPECT_FALSE(isPciRootComplex("/sys/devices/pci0000"));
}

// ==================== 三级优先级策略端到端测试 ====================

TEST(PcieTopologyTest, ThreeLevelPriorityParseTopology) {
    mooncake::Topology topology;
    
    std::string json_str = 
        "{\"gpu:0\" : [[\"mlx5_0\", \"mlx5_1\"],[\"mlx5_2\", \"mlx5_3\"]]}";
    
    topology.clear();
    int ret = topology.parse(json_str);
    ASSERT_EQ(ret, 0);
    
    auto matrix = topology.getMatrix();
    ASSERT_EQ(matrix.size(), static_cast<size_t>(1));
    ASSERT_TRUE(matrix.count("gpu:0"));
    
    auto &entry = matrix["gpu:0"];
    EXPECT_EQ(entry.preferred_hca.size(), static_cast<size_t>(2));
    EXPECT_EQ(entry.avail_hca.size(), static_cast<size_t>(2));
    
    EXPECT_EQ(entry.preferred_hca[0], "mlx5_0");
    EXPECT_EQ(entry.preferred_hca[1], "mlx5_1");
    EXPECT_EQ(entry.avail_hca[0], "mlx5_2");
    EXPECT_EQ(entry.avail_hca[1], "mlx5_3");
}

TEST(PcieTopologyTest, ThreeLevelPriorityDeviceSelection) {
    mooncake::Topology topology;
    std::string json_str = 
        "{\"gpu:0\" : [[\"mlx5_0\", \"mlx5_1\"],[\"mlx5_2\"]]}";
    
    topology.clear();
    topology.parse(json_str);
    
    std::set<int> selected_devices;
    for (int i = 0; i < 20; i++) {
        int device = topology.selectDevice("gpu:0", 0);
        selected_devices.insert(device);
        EXPECT_GE(device, 0);
        EXPECT_LT(device, 3);
    }
    
    LOG(INFO) << "Selected " << selected_devices.size() << " unique devices";
}

TEST(PcieTopologyTest, ThreeLevelPriorityRetryMechanism) {
    mooncake::Topology topology;
    std::string json_str = 
        "{\"gpu:0\" : [[\"mlx5_0\"],[\"mlx5_1\", \"mlx5_2\"]]}";
    
    topology.clear();
    topology.parse(json_str);
    
    std::vector<int> devices;
    for (int i = 1; i <= 3; i++) {
        int device = topology.selectDevice("gpu:0", i);
        devices.push_back(device);
        LOG(INFO) << "Retry " << i << " selected device: " << device;
    }
    
    EXPECT_EQ(devices.size(), static_cast<size_t>(3));
    EXPECT_EQ(devices[0], 0);
    EXPECT_TRUE(devices[1] == 1 || devices[1] == 2);
    EXPECT_TRUE(devices[2] == 1 || devices[2] == 2);
    EXPECT_NE(devices[1], devices[2]);
}

TEST(PcieTopologyTest, ThreeLevelPriorityMultipleGPUs) {
    mooncake::Topology topology;
    std::string json_str = 
        "{"
        "\"gpu:0\" : [[\"mlx5_0\", \"mlx5_1\"],[\"mlx5_2\"]],"
        "\"gpu:1\" : [[\"mlx5_2\", \"mlx5_3\"],[\"mlx5_0\"]]"
        "}";
    
    topology.clear();
    int ret = topology.parse(json_str);
    ASSERT_EQ(ret, 0);
    
    auto matrix = topology.getMatrix();
    ASSERT_EQ(matrix.size(), static_cast<size_t>(2));
    EXPECT_TRUE(matrix.count("gpu:0"));
    EXPECT_TRUE(matrix.count("gpu:1"));
    
    auto hca_list = topology.getHcaList();
    EXPECT_EQ(hca_list.size(), static_cast<size_t>(4));
}

TEST(PcieTopologyTest, ThreeLevelPriorityEmptyPreferred) {
    mooncake::Topology topology;
    std::string json_str = 
        "{\"gpu:0\" : [[],[\"mlx5_0\", \"mlx5_1\"]]}";
    
    topology.clear();
    topology.parse(json_str);
    
    int device = topology.selectDevice("gpu:0", 0);
    EXPECT_GE(device, 0);
    EXPECT_LT(device, 2);
}

TEST(PcieTopologyTest, ThreeLevelPriorityDisableDevice) {
    mooncake::Topology topology;
    std::string json_str = 
        "{\"gpu:0\" : [[\"mlx5_0\", \"mlx5_1\"],[\"mlx5_2\"]]}";
    
    topology.clear();
    topology.parse(json_str);
    
    topology.disableDevice("mlx5_0");
    
    auto matrix = topology.getMatrix();
    auto &entry = matrix["gpu:0"];
    
    EXPECT_EQ(entry.preferred_hca.size(), static_cast<size_t>(1));
    EXPECT_EQ(entry.preferred_hca[0], "mlx5_1");
    EXPECT_EQ(entry.avail_hca.size(), static_cast<size_t>(1));
}

TEST(PcieTopologyTest, ThreeLevelPriorityComplexTopology) {
    mooncake::Topology topology;
    std::string json_str = 
        "{"
        "\"gpu:0\" : [[\"mlx5_0\"],[\"mlx5_1\", \"mlx5_2\", \"mlx5_3\"]],"
        "\"gpu:1\" : [[\"mlx5_1\"],[\"mlx5_0\", \"mlx5_2\", \"mlx5_3\"]],"
        "\"cpu:0\" : [[\"mlx5_0\", \"mlx5_1\"],[\"mlx5_2\", \"mlx5_3\"]]"
        "}";
    
    topology.clear();
    int ret = topology.parse(json_str);
    ASSERT_EQ(ret, 0);
    
    auto matrix = topology.getMatrix();
    ASSERT_EQ(matrix.size(), static_cast<size_t>(3));
    
    for (int i = 0; i < 10; i++) {
        int dev_gpu0 = topology.selectDevice("gpu:0", 0);
        int dev_gpu1 = topology.selectDevice("gpu:1", 0);
        int dev_cpu0 = topology.selectDevice("cpu:0", 0);
        
        EXPECT_GE(dev_gpu0, 0);
        EXPECT_GE(dev_gpu1, 0);
        EXPECT_GE(dev_cpu0, 0);
    }
}

// ==================== isSamePcieRootComplex 集成测试 ====================

TEST(PcieTopologyTest, IsSamePcieRootComplexNullInputs) {
    EXPECT_FALSE(isSamePcieRootComplex(nullptr, "0000:17:00.0"));
    EXPECT_FALSE(isSamePcieRootComplex("0000:17:00.0", nullptr));
    EXPECT_FALSE(isSamePcieRootComplex(nullptr, nullptr));
}

TEST(PcieTopologyTest, IsSamePcieRootComplexInvalidBusIds) {
    // 无效的 bus ID 格式，realpath 会失败
    EXPECT_FALSE(isSamePcieRootComplex("invalid_bus_id", "also_invalid"));
    EXPECT_FALSE(isSamePcieRootComplex("", ""));
    EXPECT_FALSE(isSamePcieRootComplex("xyz:abc:def", "123:456:789"));
}

TEST(PcieTopologyTest, IsSamePcieRootComplexEmptyStrings) {
    EXPECT_FALSE(isSamePcieRootComplex("", "0000:17:00.0"));
    EXPECT_FALSE(isSamePcieRootComplex("0000:17:00.0", ""));
}

// ==================== 边界条件测试 ====================

TEST(PcieTopologyTest, BoundaryVeryLongPaths) {
    std::string long_path1 = "/sys/devices";
    std::string long_path2 = "/sys/devices";
    
    for (int i = 0; i < 30; i++) {
        long_path1 += "/level" + std::to_string(i);
        long_path2 += "/level" + std::to_string(i);
    }
    long_path1 += "/device1";
    long_path2 += "/device2";
    
    std::string common = getCommonParent(long_path1.c_str(), long_path2.c_str());
    EXPECT_GT(common.size(), 0UL);
    EXPECT_FALSE(common.empty());
}

TEST(PcieTopologyTest, BoundaryPathMaxLength) {
    // 测试接近 PATH_MAX 的路径
    std::string path1(PATH_MAX - 10, 'a');
    std::string path2(PATH_MAX - 10, 'a');
    
    std::string common = getCommonParent(path1.c_str(), path2.c_str());
    EXPECT_EQ(common.size(), path1.size());
}

TEST(PcieTopologyTest, BoundaryDifferentLengthPaths) {
    const char *short_path = "/sys/devices/pci0000:17";
    const char *long_path = "/sys/devices/pci0000:17/0000:17:01.0/0000:18:00.0";
    
    std::string common = getCommonParent(short_path, long_path);
    EXPECT_EQ(common, "/sys/devices/pci0000:17");
    
    // 交换顺序测试
    common = getCommonParent(long_path, short_path);
    EXPECT_EQ(common, "/sys/devices/pci0000:17");
}

TEST(PcieTopologyTest, BoundaryOnlySlashes) {
    const char *path1 = "/";
    const char *path2 = "/sys";
    
    std::string common = getCommonParent(path1, path2);
    EXPECT_TRUE(common.empty());
}

TEST(PcieTopologyTest, BoundaryConsecutiveSlashes) {
    const char *path1 = "/sys//devices//pci0000:17";
    const char *path2 = "/sys//devices//pci0000:17";
    
    std::string common = getCommonParent(path1, path2);
    EXPECT_FALSE(common.empty());
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
