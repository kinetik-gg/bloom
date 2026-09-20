// Failure-only fake tool for the GPU shader compiler adapter. Behaviour is selected by the
// invoked executable's basename, so the adapter's real argv contract is exercised without a
// shell. The success path in the companion test uses the genuine pinned glslang/spirv-val.
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>

int main(int argc, char** argv) {
    std::string name = argc > 0 && argv[0] != nullptr ? argv[0] : "";
    const auto slash = name.find_last_of('/');
    if (slash != std::string::npos)
        name = name.substr(slash + 1);
    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], "--version") == 0) {
            std::puts("fixture-tool 0");
            return 0;
        }
    }
    if (name.find("hang") != std::string::npos) {
        while (true) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
    std::string outputPath;
    for (int index = 1; index + 1 < argc; ++index) {
        if (std::strcmp(argv[index], "-o") == 0)
            outputPath = argv[index + 1];
    }
    if (name.find("valfail") != std::string::npos)
        return 1;
    if (name.find("badspv") != std::string::npos && !outputPath.empty()) {
        std::ofstream output(outputPath, std::ios::binary);
        const char junk[8] = {1, 2, 3, 4, 5, 6, 7, 8};
        output.write(junk, sizeof(junk));
        return 0;
    }
    if (name.find("badver") != std::string::npos && !outputPath.empty()) {
        std::ofstream output(outputPath, std::ios::binary);
        const unsigned char module[20] = {0x03, 0x02, 0x23, 0x07, 0x00, 0x06, 0x01, 0x00};
        output.write(reinterpret_cast<const char*>(module), sizeof(module));
        return 0;
    }
    if (name.find("noisy") != std::string::npos) {
        for (int line = 0; line < 4000; ++line)
            std::printf("noise %d\n", line);
        return 2;
    }
    return 0;
}
