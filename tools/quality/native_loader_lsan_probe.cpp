// Standalone Linux Vulkan dependency baseline. Compile with ASan/UBSan, without Bloom libraries.
// No leak suppression, DBus shutdown, persistent dlopen reference, or driver cleanup workaround.
// Native objects are destroyed before dlclose; --snapshot identifies DSOs before they unload.
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <link.h>

#include <array>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string_view>

namespace {
struct Options final {
    const char* loader = nullptr;
    std::string_view mode = "device";
    unsigned cycles = 1;
    bool snapshot = false;
};
Options parse(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--snapshot") {
            options.snapshot = true;
        } else if (index + 1 < argc && argument == "--loader") {
            options.loader = argv[++index];
        } else if (index + 1 < argc && argument == "--mode") {
            options.mode = argv[++index];
        } else if (index + 1 < argc && argument == "--cycles") {
            const std::string_view count(argv[++index]);
            const auto parsed =
                std::from_chars(count.data(), count.data() + count.size(), options.cycles);
            if (parsed.ec != std::errc{} || parsed.ptr != count.data() + count.size())
                throw std::runtime_error("invalid cycle count");
        } else {
            throw std::runtime_error("unknown or incomplete option");
        }
    }
    if (!options.loader || options.cycles == 0 || options.cycles > 8 ||
        (options.mode != "loader" && options.mode != "instance" && options.mode != "enumerate" &&
         options.mode != "device"))
        throw std::runtime_error("use --loader PATH [--mode loader|instance|enumerate|device] "
                                 "[--cycles 1..8] [--snapshot]");
    return options;
}
struct Loader final {
    void* handle;
    ~Loader() { dlclose(handle); }
};
struct Instance final {
    VkInstance value;
    PFN_vkDestroyInstance destroy;
    ~Instance() { destroy(value, nullptr); }
};
struct Device final {
    VkDevice value;
    PFN_vkDestroyDevice destroy;
    ~Device() { destroy(value, nullptr); }
};
template <class Function>
Function resolve(PFN_vkGetInstanceProcAddr get, VkInstance instance, const char* name) {
    auto function = reinterpret_cast<Function>(get(instance, name));
    if (!function)
        throw std::runtime_error("required Vulkan entry point is unavailable");
    return function;
}
void snapshot() {
    dl_iterate_phdr(
        [](dl_phdr_info* info, std::size_t, void*) {
            const char* name = std::strrchr(info->dlpi_name, '/');
            name = name ? name + 1 : info->dlpi_name;
            auto end = info->dlpi_addr;
            for (unsigned index = 0; index < info->dlpi_phnum; ++index) {
                const auto candidate = info->dlpi_addr + info->dlpi_phdr[index].p_vaddr +
                                       info->dlpi_phdr[index].p_memsz;
                if (candidate > end)
                    end = candidate;
            }
            if (*name)
                std::fprintf(stderr, "MODULE base=0x%llx end=0x%llx file=%s\n",
                             static_cast<unsigned long long>(info->dlpi_addr),
                             static_cast<unsigned long long>(end), name);
            return 0;
        },
        nullptr);
}
unsigned cycle(const Options& options) {
    void* handle = dlopen(options.loader, RTLD_NOW | RTLD_LOCAL);
    if (!handle)
        throw std::runtime_error("the explicit Vulkan loader could not be opened");
    const Loader loader{handle};
    if (options.mode == "loader") {
        if (options.snapshot)
            snapshot();
        return 0;
    }
    auto get = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(handle, "vkGetInstanceProcAddr"));
    if (!get)
        throw std::runtime_error("vkGetInstanceProcAddr is unavailable");
    const auto create = resolve<PFN_vkCreateInstance>(get, nullptr, "vkCreateInstance");
    const VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                nullptr,
                                "Native dependency baseline",
                                1,
                                "Standalone",
                                1,
                                VK_API_VERSION_1_2};
    const VkInstanceCreateInfo info{
        VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, nullptr, 0, &app, 0, nullptr, 0, nullptr};
    VkInstance rawInstance = VK_NULL_HANDLE;
    if (create(&info, nullptr, &rawInstance) != VK_SUCCESS)
        throw std::runtime_error("the required native instance could not be created");
    const Instance instance{rawInstance,
                            resolve<PFN_vkDestroyInstance>(get, rawInstance, "vkDestroyInstance")};
    if (options.mode == "instance") {
        if (options.snapshot)
            snapshot();
        return 0;
    }
    const auto enumerate =
        resolve<PFN_vkEnumeratePhysicalDevices>(get, rawInstance, "vkEnumeratePhysicalDevices");
    std::uint32_t count = 0;
    std::array<VkPhysicalDevice, 64> devices{};
    if (enumerate(rawInstance, &count, nullptr) != VK_SUCCESS || count == 0 ||
        count > devices.size() || enumerate(rawInstance, &count, devices.data()) != VK_SUCCESS)
        throw std::runtime_error("required bounded native device enumeration failed");
    if (options.mode == "enumerate") {
        if (options.snapshot)
            snapshot();
        return count;
    }
    const auto queues = resolve<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
        get, rawInstance, "vkGetPhysicalDeviceQueueFamilyProperties");
    std::uint32_t queueCount = 0;
    std::array<VkQueueFamilyProperties, 64> properties{};
    queues(devices[0], &queueCount, nullptr);
    if (queueCount == 0 || queueCount > properties.size())
        throw std::runtime_error("required bounded compute queue enumeration failed");
    queues(devices[0], &queueCount, properties.data());
    std::uint32_t family = 0;
    while (family < queueCount && (properties[family].queueFlags & VK_QUEUE_COMPUTE_BIT) == 0)
        ++family;
    if (family == queueCount)
        throw std::runtime_error("the required compute queue is unavailable");
    const float priority = 1;
    const VkDeviceQueueCreateInfo queue{
        VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, nullptr, 0, family, 1, &priority};
    const VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                                        nullptr,
                                        0,
                                        1,
                                        &queue,
                                        0,
                                        nullptr,
                                        0,
                                        nullptr,
                                        nullptr};
    VkDevice rawDevice = VK_NULL_HANDLE;
    const auto createDevice = resolve<PFN_vkCreateDevice>(get, rawInstance, "vkCreateDevice");
    if (createDevice(devices[0], &deviceInfo, nullptr, &rawDevice) != VK_SUCCESS)
        throw std::runtime_error("the required native compute device could not be created");
    const Device device{rawDevice,
                        resolve<PFN_vkDestroyDevice>(get, rawInstance, "vkDestroyDevice")};
    if (options.snapshot)
        snapshot();
    return count;
}
} // namespace
int main(int argc, char** argv) { // NOLINT(bugprone-exception-escape)
    try {
        const auto options = parse(argc, argv);
        for (unsigned index = 0; index < options.cycles; ++index) {
            const auto devices = cycle(options);
            std::fprintf(stderr,
                         "NATIVE_LOADER_PROBE cycle=%u mode=%.*s devices=%u "
                         "apiObjectsDestroyed=1\n",
                         index + 1, static_cast<int>(options.mode.size()), options.mode.data(),
                         devices);
        }
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
