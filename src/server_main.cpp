// server_main.cpp -- a standalone TCP order-entry gateway.
//
// Listens on a port, accepts client connections, and runs the single-writer
// matching engine behind them. Speak the binary protocol in include/wire.hpp.
//
// Usage: matching_server [port]   (default 9001)
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "matching_server.hpp"

namespace
{
volatile std::sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }
} // namespace

int main(int argc, char** argv)
{
    const std::uint16_t port =
        (argc > 1) ? static_cast<std::uint16_t>(std::atoi(argv[1])) : 9001;

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    MatchingServer server;
    std::uint16_t bound = server.listen(port);
    if (bound == 0)
    {
        std::fprintf(stderr, "failed to bind port %u\n", port);
        return 1;
    }
    server.start();
    std::printf("matching_server listening on 127.0.0.1:%u (Ctrl-C to stop)\n",
                bound);

    while (!g_stop) ::pause();

    std::printf("\nshutting down...\n");
    server.stop();
    return 0;
}
