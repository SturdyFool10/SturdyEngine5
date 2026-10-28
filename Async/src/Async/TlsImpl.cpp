#include <Async/Tls.hpp>

#if !defined(STURDY_PLATFORM_WEB)

#include <Async/NetInternal.hpp>

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <fstream>
#include <sstream>
#include <thread>
#include <vector>

namespace SFT::Async::Detail {

    // ---------------------------------------------------------------------------------------------
    // Reactor
    // ---------------------------------------------------------------------------------------------
    namespace {

        thread_local bool t_on_reactor = false;
        std::atomic<unsigned> g_requested_threads{0};
        std::atomic<bool> g_reactor_started{false};

        class Reactor {
          public:
            Reactor()
                : threads_count_(pick_threads()), context_(static_cast<int>(threads_count_)),
                  guard_(asio::make_work_guard(context_)) {
                g_reactor_started.store(true, std::memory_order_release);
                threads_.reserve(threads_count_);
                for (unsigned i = 0; i < threads_count_; ++i) {
                    threads_.emplace_back([this] {
                        t_on_reactor = true;
                        Async::Detail::t_callback_thread = true;
                        context_.run();
                    });
                }
            }

            ~Reactor() {
                guard_.reset();
                context_.stop();
                for (auto &thread : threads_) {
                    if (thread.joinable()) {
                        thread.join();
                    }
                }
            }

            asio::io_context &context() noexcept { return context_; }
            unsigned thread_count() const noexcept { return threads_count_; }

          private:
            static unsigned pick_threads() {
                if (const unsigned requested = g_requested_threads.load(); requested != 0) {
                    return requested;
                }
                return std::clamp(std::thread::hardware_concurrency() / 4, 2u, 4u);
            }

            unsigned threads_count_;
            asio::io_context context_;
            asio::executor_work_guard<asio::io_context::executor_type> guard_;
            std::vector<std::thread> threads_;
        };

        Reactor &instance() {
            static Reactor reactor;
            return reactor;
        }

    } // namespace

    asio::io_context &reactor() { return instance().context(); }

    bool configure_reactor(unsigned thread_count) {
        if (thread_count == 0 || thread_count > 64 || g_reactor_started.load(std::memory_order_acquire)) {
            return false;
        }
        g_requested_threads.store(thread_count);
        return true;
    }

    unsigned reactor_thread_count() { return instance().thread_count(); }
    bool on_reactor_thread() noexcept { return t_on_reactor; }

    IoError to_io_error(const char *operation, const ErrorCode &error) {
        IoErrorCode code = IoErrorCode::Unknown;
        if (error == asio::error::connection_refused) code = IoErrorCode::ConnectionRefused;
        else if (error == asio::error::connection_reset || error == asio::error::broken_pipe ||
                 error == asio::error::connection_aborted) code = IoErrorCode::ConnectionReset;
        else if (error == asio::error::timed_out) code = IoErrorCode::TimedOut;
        else if (error == asio::error::host_not_found || error == asio::error::host_not_found_try_again ||
                 error == asio::error::service_not_found) code = IoErrorCode::NotFound;
        else if (error == asio::error::access_denied) code = IoErrorCode::PermissionDenied;
        else if (error == asio::error::address_in_use) code = IoErrorCode::AlreadyExists;
        else if (error == asio::error::operation_aborted || error == asio::error::bad_descriptor) code = IoErrorCode::Closed;
        else if (error == asio::error::eof) code = IoErrorCode::UnexpectedEof;
        else if (error == asio::error::invalid_argument) code = IoErrorCode::InvalidArgument;
        else if (error == asio::error::operation_not_supported || error == asio::error::address_family_not_supported) code = IoErrorCode::Unsupported;
        else if (error.category() == asio::error::get_ssl_category() || error == asio::ssl::error::stream_truncated) code = IoErrorCode::TlsFailure;
        return IoError{code, std::string(operation) + ": " + error.message()};
    }

    // ---------------------------------------------------------------------------------------------
    // Small helpers
    // ---------------------------------------------------------------------------------------------
    std::string to_hex(const unsigned char *data, std::size_t size) {
        static constexpr char digits[] = "0123456789abcdef";
        std::string out;
        out.reserve(size * 2);
        for (std::size_t i = 0; i < size; ++i) {
            out.push_back(digits[data[i] >> 4]);
            out.push_back(digits[data[i] & 0x0F]);
        }
        return out;
    }

    std::string lowercase(std::string text) {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    }

    namespace {

        template <typename T, void (*Free)(T *)>
        struct Deleter {
            void operator()(T *p) const noexcept { Free(p); }
        };
        using KeyPtr = std::unique_ptr<EVP_PKEY, Deleter<EVP_PKEY, EVP_PKEY_free>>;
        using CertPtr = std::unique_ptr<X509, Deleter<X509, X509_free>>;
        void free_bio(BIO *bio) { BIO_free_all(bio); }
        using BioPtr = std::unique_ptr<BIO, Deleter<BIO, free_bio>>;

        std::string digest_of(X509 *certificate) {
            unsigned char digest[EVP_MAX_MD_SIZE];
            unsigned int length = 0;
            if (X509_digest(certificate, EVP_sha256(), digest, &length) != 1) {
                return {};
            }
            return to_hex(digest, length);
        }

        CertPtr parse_certificate(const std::string &pem) {
            BioPtr bio(BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())));
            if (!bio) return nullptr;
            return CertPtr(PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr));
        }

        std::string bio_to_string(BIO *bio) {
            char *data = nullptr;
            const long size = BIO_get_mem_data(bio, &data);
            return size > 0 ? std::string(data, static_cast<std::size_t>(size)) : std::string{};
        }

        /// ALPN protocol list in the wire format OpenSSL wants: length-prefixed strings.
        Result<std::vector<unsigned char>> alpn_wire(const std::vector<std::string> &protocols) {
            std::vector<unsigned char> wire;
            for (const std::string &protocol : protocols) {
                if (protocol.empty() || protocol.size() > 255) {
                    return std::unexpected(IoError{IoErrorCode::InvalidArgument, "ALPN protocol names must be 1-255 bytes"});
                }
                wire.push_back(static_cast<unsigned char>(protocol.size()));
                wire.insert(wire.end(), protocol.begin(), protocol.end());
            }
            return wire;
        }

        IoError tls_setup_error(const char *what) {
            return IoError{IoErrorCode::InvalidArgument, std::string("TLS configuration: ") + what};
        }

        /// Everything a server context needs to outlive: the context plus the data its callbacks point at.
        struct ServerContextHolder {
            asio::ssl::context context{asio::ssl::context::tls_server};
            std::vector<unsigned char> alpn;
        };

        int select_alpn(SSL *, const unsigned char **out, unsigned char *out_length, const unsigned char *in, unsigned in_length,
                        void *argument) {
            const auto &ours = *static_cast<const std::vector<unsigned char> *>(argument);
            if (SSL_select_next_proto(const_cast<unsigned char **>(out), out_length, ours.data(), static_cast<unsigned>(ours.size()), in,
                                      in_length) == OPENSSL_NPN_NEGOTIATED) {
                return SSL_TLSEXT_ERR_OK;
            }
            return SSL_TLSEXT_ERR_ALERT_FATAL;
        }

        int accept_any_certificate(int, X509_STORE_CTX *) { return 1; }

    } // namespace

    // ---------------------------------------------------------------------------------------------
    // TLS contexts
    // ---------------------------------------------------------------------------------------------
    Result<std::shared_ptr<asio::ssl::context>> make_tls_client_context(const TlsClientOptions &options) {
        try {
            auto context = std::make_shared<asio::ssl::context>(asio::ssl::context::tls_client);
            context->set_options(asio::ssl::context::default_workarounds | asio::ssl::context::no_sslv2 | asio::ssl::context::no_sslv3 |
                                 asio::ssl::context::no_tlsv1 | asio::ssl::context::no_tlsv1_1);
            if (SSL_CTX_set_min_proto_version(context->native_handle(), TLS1_2_VERSION) != 1) {
                return std::unexpected(tls_setup_error("cannot enforce TLS 1.2 minimum"));
            }
            const bool pinned = !options.pinned_sha256.empty() || options.identify_peer_by_certificate_only;
            if (pinned) {
                // The exact-certificate pin (or the caller's own fingerprint check, for identity-only mode) replaces chain
                // validation; a pin is verified right after the handshake.
                SSL_CTX_set_verify(context->native_handle(), SSL_VERIFY_PEER, accept_any_certificate);
            } else {
                if (options.use_system_roots) {
                    context->set_default_verify_paths();
                }
                if (!options.ca_certificates_pem.empty()) {
                    context->add_certificate_authority(asio::buffer(options.ca_certificates_pem));
                }
                context->set_verify_mode(asio::ssl::verify_peer);
            }
            if (options.client_certificate_chain_pem.empty() != options.client_private_key_pem.empty()) {
                return std::unexpected(tls_setup_error("client certificate and key must be given together"));
            }
            if (!options.client_certificate_chain_pem.empty()) {
                context->use_certificate_chain(asio::buffer(options.client_certificate_chain_pem));
                context->use_private_key(asio::buffer(options.client_private_key_pem), asio::ssl::context::pem);
                if (SSL_CTX_check_private_key(context->native_handle()) != 1) {
                    return std::unexpected(tls_setup_error("client private key does not match its certificate"));
                }
            }
            if (!options.alpn.empty()) {
                auto wire = alpn_wire(options.alpn);
                if (!wire) return std::unexpected(wire.error());
                if (SSL_CTX_set_alpn_protos(context->native_handle(), wire->data(), static_cast<unsigned>(wire->size())) != 0) {
                    return std::unexpected(tls_setup_error("cannot set ALPN protocols"));
                }
            }
            return context;
        } catch (const std::exception &error) {
            return std::unexpected(IoError{IoErrorCode::TlsFailure, std::string("TLS client setup failed: ") + error.what()});
        }
    }

    Result<std::shared_ptr<asio::ssl::context>> make_tls_server_context(const TlsServerOptions &options) {
        try {
            if (options.certificate_chain_pem.empty() || options.private_key_pem.empty()) {
                return std::unexpected(tls_setup_error("server certificate chain and private key are required"));
            }
            auto holder = std::make_shared<ServerContextHolder>();
            auto &context = holder->context;
            context.set_options(asio::ssl::context::default_workarounds | asio::ssl::context::no_sslv2 | asio::ssl::context::no_sslv3 |
                                asio::ssl::context::no_tlsv1 | asio::ssl::context::no_tlsv1_1 | asio::ssl::context::single_dh_use);
            if (SSL_CTX_set_min_proto_version(context.native_handle(), TLS1_2_VERSION) != 1) {
                return std::unexpected(tls_setup_error("cannot enforce TLS 1.2 minimum"));
            }
            context.use_certificate_chain(asio::buffer(options.certificate_chain_pem));
            context.use_private_key(asio::buffer(options.private_key_pem), asio::ssl::context::pem);
            if (SSL_CTX_check_private_key(context.native_handle()) != 1) {
                return std::unexpected(tls_setup_error("private key does not match the certificate"));
            }

            const bool pinned = !options.pinned_client_sha256.empty() || options.accept_any_client_certificate;
            const bool has_ca = !options.client_ca_pem.empty();
            if (options.require_client_certificate && !pinned && !has_ca) {
                return std::unexpected(tls_setup_error("require_client_certificate needs client_ca_pem, pinned_client_sha256 or accept_any_client_certificate"));
            }
            if (pinned) {
                // Pinning names the only acceptable certificates, so a client without one is always refused.
                SSL_CTX_set_verify(context.native_handle(), SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, accept_any_certificate);
            } else if (has_ca) {
                context.add_certificate_authority(asio::buffer(options.client_ca_pem));
                int mode = SSL_VERIFY_PEER;
                if (options.require_client_certificate) mode |= SSL_VERIFY_FAIL_IF_NO_PEER_CERT;
                SSL_CTX_set_verify(context.native_handle(), mode, nullptr);
            }
            if (!options.alpn.empty()) {
                auto wire = alpn_wire(options.alpn);
                if (!wire) return std::unexpected(wire.error());
                holder->alpn = std::move(*wire);
                SSL_CTX_set_alpn_select_cb(context.native_handle(), select_alpn, &holder->alpn);
            }
            return std::shared_ptr<asio::ssl::context>(holder, &holder->context);
        } catch (const std::exception &error) {
            return std::unexpected(IoError{IoErrorCode::TlsFailure, std::string("TLS server setup failed: ") + error.what()});
        }
    }

    Result<void> prepare_tls_client(SSL *ssl, const TlsClientOptions &options, const std::string &connect_host) {
        const std::string name = options.server_name.empty() ? connect_host : options.server_name;
        boost::system::error_code ip_error;
        (void)asio::ip::make_address(name, ip_error);
        if (!name.empty() && ip_error) { // SNI carries host names only, never IP literals
            (void)SSL_set_tlsext_host_name(ssl, name.c_str());
        }
        if (options.pinned_sha256.empty() && !options.identify_peer_by_certificate_only) {
            if (name.empty()) {
                return std::unexpected(tls_setup_error("a server name is required to verify the certificate"));
            }
            if (!ip_error) {
                if (X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(ssl), name.c_str()) != 1) {
                    return std::unexpected(tls_setup_error("invalid IP address for certificate verification"));
                }
            } else {
                X509_VERIFY_PARAM_set_hostflags(SSL_get0_param(ssl), X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
                if (SSL_set1_host(ssl, name.c_str()) != 1) {
                    return std::unexpected(tls_setup_error("invalid host name for certificate verification"));
                }
            }
        }
        return {};
    }

    Result<void> check_certificate_pin(SSL *ssl, const std::vector<std::string> &pins) {
        if (pins.empty()) {
            return {};
        }
        std::unique_ptr<X509, Deleter<X509, X509_free>> peer(SSL_get1_peer_certificate(ssl));
        if (!peer) {
            return std::unexpected(IoError{IoErrorCode::TlsFailure, "peer presented no certificate"});
        }
        const std::string actual = digest_of(peer.get());
        for (const std::string &pin : pins) {
            if (lowercase(pin) == actual) {
                return {};
            }
        }
        return std::unexpected(IoError{IoErrorCode::TlsFailure, "peer certificate does not match any pinned SHA-256 (got " + actual + ")"});
    }

    TlsInfo describe_tls(SSL *ssl) {
        TlsInfo info;
        if (const char *version = SSL_get_version(ssl)) info.protocol = version;
        if (const SSL_CIPHER *cipher = SSL_get_current_cipher(ssl)) {
            if (const char *name = SSL_CIPHER_get_name(cipher)) info.cipher = name;
        }
        const unsigned char *selected = nullptr;
        unsigned selected_length = 0;
        SSL_get0_alpn_selected(ssl, &selected, &selected_length);
        if (selected != nullptr) info.alpn.assign(reinterpret_cast<const char *>(selected), selected_length);
        std::unique_ptr<X509, Deleter<X509, X509_free>> peer(SSL_get1_peer_certificate(ssl));
        if (peer) info.peer_sha256 = digest_of(peer.get());
        return info;
    }

} // namespace SFT::Async::Detail

namespace SFT::Async {

    std::expected<TlsIdentity, IoError> generate_self_signed_identity(const std::string &common_name, unsigned valid_days) {
        using namespace Detail;
        if (common_name.empty() || common_name.size() > 64 || valid_days == 0 || valid_days > 36500) {
            return std::unexpected(IoError{IoErrorCode::InvalidArgument, "identity needs a 1-64 byte name and 1-36500 valid days"});
        }
        KeyPtr key(EVP_EC_gen("P-256"));
        CertPtr certificate(X509_new());
        if (!key || !certificate) {
            return std::unexpected(IoError{IoErrorCode::Unknown, "OpenSSL could not allocate key material"});
        }
        unsigned char serial_bytes[8];
        if (RAND_bytes(serial_bytes, sizeof(serial_bytes)) != 1) {
            return std::unexpected(IoError{IoErrorCode::Unknown, "OpenSSL random generator failed"});
        }
        serial_bytes[0] &= 0x7F;
        BIGNUM *serial_number = BN_bin2bn(serial_bytes, sizeof(serial_bytes), nullptr);
        const bool serial_ok = serial_number != nullptr && BN_to_ASN1_INTEGER(serial_number, X509_get_serialNumber(certificate.get())) != nullptr;
        BN_free(serial_number);
        X509_NAME *name = X509_get_subject_name(certificate.get());
        const std::string san = "DNS:" + common_name;
        X509V3_CTX extension_context;
        X509V3_set_ctx_nodb(&extension_context);
        X509V3_set_ctx(&extension_context, certificate.get(), certificate.get(), nullptr, nullptr, 0);
        X509_EXTENSION *san_extension = X509V3_EXT_conf_nid(nullptr, &extension_context, NID_subject_alt_name, san.c_str());
        const bool ok = serial_ok && X509_set_version(certificate.get(), 2) == 1 &&
                        X509_gmtime_adj(X509_getm_notBefore(certificate.get()), -60) != nullptr &&
                        X509_gmtime_adj(X509_getm_notAfter(certificate.get()), static_cast<long>(valid_days) * 86400L) != nullptr &&
                        X509_set_pubkey(certificate.get(), key.get()) == 1 &&
                        X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_UTF8, reinterpret_cast<const unsigned char *>(common_name.data()),
                                                   static_cast<int>(common_name.size()), -1, 0) == 1 &&
                        X509_set_issuer_name(certificate.get(), name) == 1 && san_extension != nullptr &&
                        X509_add_ext(certificate.get(), san_extension, -1) == 1 &&
                        X509_sign(certificate.get(), key.get(), EVP_sha256()) > 0;
        if (san_extension != nullptr) X509_EXTENSION_free(san_extension);
        if (!ok) {
            return std::unexpected(IoError{IoErrorCode::Unknown, "OpenSSL could not build the certificate"});
        }
        BioPtr certificate_bio(BIO_new(BIO_s_mem()));
        BioPtr key_bio(BIO_new(BIO_s_mem()));
        if (!certificate_bio || !key_bio || PEM_write_bio_X509(certificate_bio.get(), certificate.get()) != 1 ||
            PEM_write_bio_PrivateKey(key_bio.get(), key.get(), nullptr, nullptr, 0, nullptr, nullptr) != 1) {
            return std::unexpected(IoError{IoErrorCode::Unknown, "OpenSSL could not encode the identity"});
        }
        return TlsIdentity{bio_to_string(certificate_bio.get()), bio_to_string(key_bio.get()), digest_of(certificate.get())};
    }

    std::expected<std::string, IoError> certificate_sha256(const std::string &certificate_pem) {
        auto certificate = Detail::parse_certificate(certificate_pem);
        if (!certificate) {
            return std::unexpected(IoError{IoErrorCode::InvalidArgument, "not a PEM certificate"});
        }
        return Detail::digest_of(certificate.get());
    }

    std::expected<std::string, IoError> read_text_file(const std::string &path) {
        std::ifstream file(path, std::ios::binary);
        if (!file) {
            return std::unexpected(IoError{IoErrorCode::NotFound, "cannot open '" + path + "'"});
        }
        std::ostringstream contents;
        contents << file.rdbuf();
        return contents.str();
    }

} // namespace SFT::Async

#else // Web: no OpenSSL; the helpers that need it report Unsupported (reading a file still works through the virtual file system).

#include <fstream>
#include <sstream>

namespace SFT::Async {

    std::expected<TlsIdentity, IoError> generate_self_signed_identity(const std::string &, unsigned) {
        return std::unexpected(IoError{IoErrorCode::Unsupported, "certificate generation is not available on Web"});
    }

    std::expected<std::string, IoError> certificate_sha256(const std::string &) {
        return std::unexpected(IoError{IoErrorCode::Unsupported, "certificate handling is not available on Web"});
    }

    std::expected<std::string, IoError> read_text_file(const std::string &path) {
        std::ifstream file(path, std::ios::binary);
        if (!file) return std::unexpected(IoError{IoErrorCode::NotFound, "cannot open '" + path + "'"});
        std::ostringstream contents;
        contents << file.rdbuf();
        return contents.str();
    }

} // namespace SFT::Async

#endif
