#include "../src/CtlServer.hpp"
#include "../src/Platform.h"
#include <cstdlib>
int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    CtlServer server;
    if (!server.Start(std::atoi(argv[1]))) return 3;
    bool done = false;
    while (!done)
    {
        int conn;
        std::string line;
        if (server.Poll(&conn, &line))
        {
            done = line.find("quit") != std::string::npos;
            server.Reply(conn, "{\"ok\":true,\"executed\":true}");
        }
        plat::SleepMs(5);
    }
    server.Stop(); // Also exercises shutdown while authenticated and unauthenticated readers are alive.
}
