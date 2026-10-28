#include <Async/HttpServer.hpp>
#include <Async/Scheduler.hpp>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

int main() {
    using namespace SFT::Async;
    using namespace SFT::Async::Literals;
    namespace asio = boost::asio;
    namespace beast = boost::beast;
    namespace http = beast::http;
    using tcp = asio::ip::tcp;

    Scheduler::initialize(2);
    std::atomic<unsigned> handled{0};
    const auto caller_thread = std::this_thread::get_id();
    std::atomic<bool> handler_was_separate{false};
    HttpServerOptions options;
    options.bind_address = "127.0.0.1";
    options.port = 0;
    options.io_threads = 2;
    options.handler_threads = 2;
    options.max_connections = 8;
    options.max_request_body_bytes = 1_KiB;
    options.max_response_body_bytes = 64_B;

    HttpServer server(options, HttpServer::AsyncHandler{[&](HttpRequestMessage request) {
        return Scheduler::spawn([request = std::move(request), &handled, &handler_was_separate, caller_thread] {
            ++handled;
            handler_was_separate.store(std::this_thread::get_id() != caller_thread, std::memory_order_release);
            HttpResponseMessage response;
            response.status = 200;
            response.headers.push_back({"Content-Type", "text/plain"});
            response.body = request.target == "/large" ? std::string(100, 'x') :
                request.method + ":" + request.target + ":" + request.body;
            return response;
        });
    }});

    auto started = server.start();
    if (!started) {
        std::cerr << "HTTP server failed to start: " << started.error().message << '\n';
        return 1;
    }
    auto port = server.local_port();
    if (!port || *port == 0 || !server.is_running() || server.uses_tls()) {
        std::cerr << "HTTP server reported invalid listener state\n";
        server.stop();
        return 1;
    }

    boost::system::error_code error;
    asio::io_context context;
    tcp::socket socket(context);
    socket.connect({asio::ip::make_address("127.0.0.1"), *port}, error);
    if (error) {
        std::cerr << "HTTP client connect failed: " << error.message() << '\n';
        server.stop();
        return 1;
    }

    bool plain_exchange_succeeded = true;
    for (unsigned index = 0; index < 2; ++index) {
        http::request<http::string_body> request{http::verb::get, index == 0 ? "/first" : "/second", 11};
        request.set(http::field::host, "localhost");
        request.keep_alive(index == 0);
        if (index == 1) request.body() = "payload";
        request.prepare_payload();
        http::write(socket, request, error);
        if (error) {
            plain_exchange_succeeded = false;
            break;
        }

        beast::flat_buffer buffer;
        http::response<http::string_body> response;
        http::read(socket, buffer, response, error);
        if (error || response.result() != http::status::ok) {
            plain_exchange_succeeded = false;
            break;
        }
        const std::string expected = index == 0 ? "GET:/first:" : "GET:/second:payload";
        if (response.body() != expected) {
            std::cerr << "unexpected HTTP response body: " << response.body() << '\n';
            plain_exchange_succeeded = false;
            break;
        }
    }
    socket.close(error);

    tcp::socket oversized_client(context);
    oversized_client.connect({asio::ip::make_address("127.0.0.1"), *port}, error);
    http::request<http::string_body> oversized_request{http::verb::get, "/large", 11};
    oversized_request.set(http::field::host, "localhost");
    oversized_request.keep_alive(false);
    if (!error) http::write(oversized_client, oversized_request, error);
    beast::flat_buffer oversized_buffer;
    http::response<http::string_body> oversized_response;
    if (!error) http::read(oversized_client, oversized_buffer, oversized_response, error);
    const bool oversized_response_rejected = !error &&
        oversized_response.result() == http::status::internal_server_error;
    oversized_client.close(error);

    tcp::socket idle_socket(context);
    idle_socket.connect({asio::ip::make_address("127.0.0.1"), *port}, error);
    const bool idle_connected = !error;
    if (idle_connected) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    server.stop();
    std::array<char, 1> closed_probe{};
    boost::system::error_code idle_read_error;
    idle_socket.read_some(asio::buffer(closed_probe), idle_read_error);
    if (!plain_exchange_succeeded || !oversized_response_rejected || handled.load(std::memory_order_acquire) != 3 ||
        !handler_was_separate.load(std::memory_order_acquire) || server.is_running() || !idle_connected || !idle_read_error) {
        std::cerr << "HTTP keep-alive, handler dispatch, or stop validation failed\n";
        return 1;
    }

    HttpServerOptions invalid_tls_options;
    invalid_tls_options.bind_address = "127.0.0.1";
    invalid_tls_options.tls_certificate_chain_file = "missing-test-cert.pem";
    HttpServer invalid_tls_server(std::move(invalid_tls_options), [](HttpRequestMessage) { return HttpResponseMessage{}; });
    if (invalid_tls_server.start()) {
        invalid_tls_server.stop();
        std::cerr << "TLS server started without a matching private key\n";
        return 1;
    }

    const auto test_directory = std::filesystem::path(__FILE__).parent_path();
    HttpServerOptions https_options;
    https_options.bind_address = "127.0.0.1";
    https_options.port = 0;
    https_options.tls_certificate_chain_file = (test_directory / "test-server-cert.pem").string();
    https_options.tls_private_key_file = (test_directory / "test-server-key.pem").string();
    HttpServer https_server(std::move(https_options), [](HttpRequestMessage) {
        return HttpResponseMessage{200, {}, "secure", false};
    });
    auto https_started = https_server.start();
    if (!https_started || !https_server.uses_tls()) {
        std::cerr << "HTTPS server failed to start with the test certificate\n";
        return 1;
    }
    auto https_port = https_server.local_port();
    if (!https_port) {
        https_server.stop();
        std::cerr << "HTTPS server did not expose its bound port\n";
        return 1;
    }

    asio::ssl::context client_tls(asio::ssl::context::tls_client);
    client_tls.set_verify_mode(asio::ssl::verify_none);
    beast::ssl_stream<beast::tcp_stream> secure_socket(context, client_tls);
    beast::get_lowest_layer(secure_socket).connect({asio::ip::make_address("127.0.0.1"), *https_port}, error);
    if (!error) secure_socket.handshake(asio::ssl::stream_base::client, error);
    if (!error) {
        http::request<http::string_body> secure_request{http::verb::get, "/tls", 11};
        secure_request.set(http::field::host, "localhost");
        secure_request.keep_alive(false);
        http::write(secure_socket, secure_request, error);
    }
    beast::flat_buffer secure_buffer;
    http::response<http::string_body> secure_response;
    if (!error) http::read(secure_socket, secure_buffer, secure_response, error);
    secure_socket.shutdown(error);
    https_server.stop();
    if (error || secure_response.result() != http::status::ok || secure_response.body() != "secure") {
        std::cerr << "HTTPS request failed: " << error.message() << '\n';
        Scheduler::shutdown();
        return 1;
    }
    Scheduler::shutdown();
    return 0;
}
