#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

#include <libwebsockets.h>

static std::atomic<bool> g_done{false};

static int ws_callback(struct lws *wsi, enum lws_callback_reasons reason, void *user, void *in, size_t len) {
    switch (reason) {
    case LWS_CALLBACK_CLIENT_ESTABLISHED:
        std::cout << "Connected to server" << std::endl;
        break;
    case LWS_CALLBACK_CLIENT_RECEIVE: {
        std::string msg(reinterpret_cast<char *>(in), len);
        std::cout << "Received: " << msg << std::endl;
        break;
    }
    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
        std::cerr << "Connection error" << std::endl;
        g_done = true;
        break;
    case LWS_CALLBACK_CLIENT_CLOSED:
        std::cout << "Connection closed" << std::endl;
        g_done = true;
        break;
    default:
        break;
    }
    return 0;
}

static struct lws_protocols protocols[] = {{"rtd-protocol", ws_callback, 0, 65536}, {nullptr, nullptr, 0, 0}};

int main(int argc, char **argv) {
    const char *address = "127.0.0.1";
    int port = 8080;
    const char *path = "/stream";

    struct lws_context_creation_info info;
    memset(&info, 0, sizeof(info));
    info.port = CONTEXT_PORT_NO_LISTEN;
    info.protocols = protocols;
    info.options = 0;

    struct lws_context *context = lws_create_context(&info);
    if (!context) {
        std::cerr << "Failed to create lws context" << std::endl;
        return 1;
    }

    struct lws_client_connect_info ccinfo;
    memset(&ccinfo, 0, sizeof(ccinfo));
    ccinfo.context = context;
    ccinfo.address = address;
    ccinfo.port = port;
    ccinfo.path = path;
    ccinfo.host = address;
    ccinfo.origin = address;
    ccinfo.protocol = protocols[0].name;
    ccinfo.ssl_connection = 0; // 0 for ws, LCCSCF_USE_SSL for wss

    struct lws *wsi = lws_client_connect_via_info(&ccinfo);
    if (!wsi) {
        std::cerr << "Client connection failed" << std::endl;
        lws_context_destroy(context);
        return 1;
    }

    // Run service loop until interrupted or timeout
    auto start = std::chrono::steady_clock::now();
    const auto maxDuration = std::chrono::seconds(30);
    while (!g_done) {
        lws_service(context, 100);
        if (std::chrono::steady_clock::now() - start > maxDuration) {
            std::cout << "Timeout reached, exiting" << std::endl;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    lws_context_destroy(context);
    return 0;
}