#include "server.hpp"
#include "transport.hpp"

#include <iostream>
#include <string>
#include <string_view>

int main(int argc, char** argv) {
    try {
        if (argc != 1 && !(argc == 3 && std::string_view(argv[1]) == "--project")) {
            std::cerr << "usage: bloom-mcp [--project <file>]\n";
            return 2;
        }
        auto opened = argc == 3 ? bloom::scripting::Session::open(argv[2])
                                : bloom::scripting::Session::createNew();
        if (!opened) {
            std::cerr << opened.diagnostic().message << '\n';
            return 2;
        }
        bloom::mcp::Server server(std::move(opened).takeSession());
        bloom::mcp::serveStdio(server);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "bloom-mcp: " << error.what() << '\n';
        return 1;
    }
}
