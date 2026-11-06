#include "vexfs_nfs_gateway.h"
#include "vexfs_runtime_types.h"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

struct Options {
    std::string backend;
    std::string connection;
    std::string workspace;
    std::string principal;
    std::string listen_address = "127.0.0.1";
    uint16_t port = 0;
    uint32_t timeout_ms = 30'000;
};

uint32_t ParseUnsigned(const char *value, uint32_t maximum, const char *name) {
    char *end = nullptr;
    const unsigned long parsed = std::strtoul(value, &end, 10);
    if (end == value || *end != '\0' || parsed == 0 || parsed > maximum)
        throw std::runtime_error(std::string("invalid ") + name);
    return static_cast<uint32_t>(parsed);
}

Options Parse(int argc, char **argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (index + 1 >= argc) throw std::runtime_error("missing value for " + argument);
        const char *value = argv[++index];
        if (argument == "--backend") options.backend = value;
        else if (argument == "--connection") options.connection = value;
        else if (argument == "--workspace") options.workspace = value;
        else if (argument == "--principal") options.principal = value;
        else if (argument == "--listen") options.listen_address = value;
        else if (argument == "--port")
            options.port = static_cast<uint16_t>(ParseUnsigned(value, 65535, "port"));
        else if (argument == "--timeout-ms")
            options.timeout_ms = ParseUnsigned(value, 3'600'000, "timeout");
        else throw std::runtime_error("unknown argument: " + argument);
    }
    if (options.backend == VEXFS_RUNTIME_BACKEND_SQLITE && options.principal.empty())
        options.principal = "local";
    if (options.backend.empty() || options.connection.empty() ||
        options.workspace.empty() || options.port == 0)
        throw std::runtime_error(
            "backend, connection, workspace and port are required");
    if (options.listen_address != "127.0.0.1")
        throw std::runtime_error("NFS gateway must listen on 127.0.0.1");
    return options;
}

}  // namespace

int main(int argc, char **argv) {
    try {
        const Options options = Parse(argc, argv);
        const vexfs_nfs_gateway_config config{
            VEXFS_RUNTIME_ABI_VERSION,
            options.backend.c_str(),
            options.connection.c_str(),
            options.workspace.c_str(),
            options.principal.c_str(),
            options.listen_address.c_str(),
            options.port,
            options.timeout_ms,
        };
        char error[1024]{};
        const int result = vexfs_nfs_gateway_run(&config, error, sizeof(error));
        if (result != 0)
            std::cerr << "vexfs-nfs-gateway: "
                      << (error[0] == '\0' ? "gateway failed" : error) << '\n';
        return result;
    } catch (const std::exception &error) {
        std::cerr << "vexfs-nfs-gateway: " << error.what() << '\n';
        return 2;
    }
}
