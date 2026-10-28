/// C ABI for the engine's network / file / process / parallelism layer.
///
/// A thin translation of `Async/Binding.hpp`: every function validates its arguments, converts the C structs to the facade's plain types,
/// calls the facade, and turns the result into a handle. Operations (`SturdyAsyncOp`) and objects (`SturdyAsyncObject`) are owned by
/// the caller and live until released; the tables below hold the shared references so a release racing with a completion callback can
/// never leave a dangling pointer.

#include <FFI/AsyncNetSupport.hpp>

#include <Async/Scheduler.hpp>

using namespace SFT::Ffi::AsyncSupport;

namespace SFT::Ffi::AsyncSupport {

    std::mutex g_mutex;
    std::unordered_map<u64, std::unique_ptr<OpEntry>> &g_ops = *new std::unordered_map<u64, std::unique_ptr<OpEntry>>();
    std::unordered_map<u64, std::unique_ptr<B::ObjectPtr>> &g_objects = *new std::unordered_map<u64, std::unique_ptr<B::ObjectPtr>>();
    std::unordered_map<u64, std::unique_ptr<B::HttpRespond>> &g_responders = *new std::unordered_map<u64, std::unique_ptr<B::HttpRespond>>();

    SturdyResult publish_op(B::OpPtr op, SturdyAsyncOp *out) {
        auto entry = std::make_unique<OpEntry>();
        entry->op = std::move(op);
        void *pointer = entry.get();
        const u64 token = mint_handle(HandleKind::AsyncOp, pointer);
        {
            const std::lock_guard<std::mutex> lock{g_mutex};
            g_ops.emplace(token, std::move(entry));
        }
        out->token = token;
        return STURDY_OK;
    }

    u64 publish_object_token(B::ObjectPtr object) {
        auto owned = std::make_unique<B::ObjectPtr>(std::move(object));
        void *pointer = owned.get();
        const u64 token = mint_handle(HandleKind::AsyncObject, pointer);
        const std::lock_guard<std::mutex> lock{g_mutex};
        g_objects.emplace(token, std::move(owned));
        return token;
    }

    SturdyAsyncObject publish_object(B::ObjectPtr object) {
        if (!object) return SturdyAsyncObject{0};
        return SturdyAsyncObject{publish_object_token(std::move(object))};
    }

    SturdyResult resolve_op(SturdyAsyncOp handle, B::OpPtr *out, u64 *object_token) noexcept {
        void *pointer = nullptr;
        const SturdyResult resolved = resolve_handle(handle.token, HandleKind::AsyncOp, &pointer);
        if (resolved != STURDY_OK) return resolved;
        const std::lock_guard<std::mutex> lock{g_mutex};
        auto it = g_ops.find(handle.token);
        if (it == g_ops.end()) return set_error(STURDY_ERROR_HANDLE_EXPIRED, "the operation was released");
        *out = it->second->op;
        if (object_token != nullptr) *object_token = it->second->object_token;
        return STURDY_OK;
    }

    SturdyResult resolve_object(SturdyAsyncObject handle, B::ObjectPtr *out) noexcept {
        void *pointer = nullptr;
        const SturdyResult resolved = resolve_handle(handle.token, HandleKind::AsyncObject, &pointer);
        if (resolved != STURDY_OK) return resolved;
        const std::lock_guard<std::mutex> lock{g_mutex};
        auto it = g_objects.find(handle.token);
        if (it == g_objects.end()) return set_error(STURDY_ERROR_HANDLE_EXPIRED, "the object was released");
        *out = *it->second;
        return STURDY_OK;
    }

    namespace {
        SturdyResult publish_finished(B::Value value, SturdyAsyncOp *out) {
            auto op = std::make_shared<B::Op>();
            value.status = B::OpStatus::Ok;
            op->complete(std::move(value));
            return publish_op(std::move(op), out);
        }
    } // namespace

    SturdyResult publish_fields(B::Fields fields, SturdyAsyncOp *out) {
        B::Value value;
        value.fields = std::move(fields);
        return publish_finished(std::move(value), out);
    }
    SturdyResult publish_bytes(B::Bytes bytes, SturdyAsyncOp *out) {
        B::Value value;
        value.bytes = std::move(bytes);
        return publish_finished(std::move(value), out);
    }
    SturdyResult publish_number(std::int64_t number, SturdyAsyncOp *out) {
        B::Value value;
        value.number = number;
        return publish_finished(std::move(value), out);
    }
    SturdyResult publish_text(std::string text, SturdyAsyncOp *out) {
        B::Value value;
        value.text = std::move(text);
        return publish_finished(std::move(value), out);
    }

    B::TlsClientSpec tls_of(const SturdyTlsClientOptions &o) {
        B::TlsClientSpec s;
        s.server_name = text(o.server_name);
        s.use_system_roots = o.no_system_roots == STURDY_FALSE;
        s.ca_pem = text(o.ca_pem);
        s.pinned_sha256 = strings(o.pinned_sha256, o.pinned_sha256_count);
        s.identify_peer_by_certificate_only = o.identify_peer_by_certificate_only != STURDY_FALSE;
        s.client_certificate_pem = text(o.client_certificate_pem);
        s.client_private_key_pem = text(o.client_private_key_pem);
        s.alpn = strings(o.alpn, o.alpn_count);
        if (o.handshake_timeout_ms != 0) s.handshake_timeout_ms = o.handshake_timeout_ms;
        return s;
    }
    B::TlsServerSpec tls_of(const SturdyTlsServerOptions &o) {
        B::TlsServerSpec s;
        s.certificate_pem = text(o.certificate_pem);
        s.private_key_pem = text(o.private_key_pem);
        s.client_ca_pem = text(o.client_ca_pem);
        s.require_client_certificate = o.require_client_certificate != STURDY_FALSE;
        s.accept_any_client_certificate = o.accept_any_client_certificate != STURDY_FALSE;
        s.pinned_client_sha256 = strings(o.pinned_client_sha256, o.pinned_client_sha256_count);
        s.alpn = strings(o.alpn, o.alpn_count);
        if (o.handshake_timeout_ms != 0) s.handshake_timeout_ms = o.handshake_timeout_ms;
        return s;
    }
    B::WebSocketSpec socket_of(const SturdyWebSocketOptions &o) {
        B::WebSocketSpec s;
        if (o.max_message_bytes != 0) s.max_message_bytes = o.max_message_bytes;
        if (o.max_queued_send_bytes != 0) s.max_queued_send_bytes = o.max_queued_send_bytes;
        if (o.idle_timeout_ms != 0) s.idle_timeout_ms = o.idle_timeout_ms;
        s.compression = o.compression != STURDY_FALSE;
        if (o.inbox_messages != 0) s.inbox_messages = o.inbox_messages;
        if (o.handshake_timeout_ms != 0) s.handshake_timeout_ms = o.handshake_timeout_ms;
        return s;
    }
    B::QuicLimitsSpec limits_of(const SturdyQuicLimits &o) {
        B::QuicLimitsSpec s;
        if (o.idle_timeout_ms != 0) s.idle_timeout_ms = o.idle_timeout_ms;
        s.keep_alive_ms = o.keep_alive_ms;
        if (o.handshake_timeout_ms != 0) s.handshake_timeout_ms = o.handshake_timeout_ms;
        if (o.max_bidirectional_streams != 0) s.max_bidirectional_streams = o.max_bidirectional_streams;
        if (o.max_unidirectional_streams != 0) s.max_unidirectional_streams = o.max_unidirectional_streams;
        if (o.stream_receive_window != 0) s.stream_receive_window = o.stream_receive_window;
        if (o.connection_receive_window != 0) s.connection_receive_window = o.connection_receive_window;
        s.datagrams = o.no_datagrams == STURDY_FALSE;
        if (o.stream_send_buffer != 0) s.stream_send_buffer = o.stream_send_buffer;
        if (o.datagram_receive_queue != 0) s.datagram_receive_queue = o.datagram_receive_queue;
        if (o.datagram_send_queue != 0) s.datagram_send_queue = o.datagram_send_queue;
        return s;
    }
    B::PortMapSpec portmap_of(const SturdyPortMapOptions *o) {
        B::PortMapSpec s;
        if (o == nullptr) return s;
        s.gateway = text(o->gateway);
        if (o->lifetime_seconds != 0) s.lifetime_s = o->lifetime_seconds;
        s.external_port = o->external_port;
        if (o->timeout_ms != 0) s.timeout_ms = o->timeout_ms;
        if (o->methods != 0) s.methods = o->methods;
        if (o->gateway_port != 0) s.gateway_port = o->gateway_port;
        if (o->ssdp_host != nullptr) s.ssdp_host = o->ssdp_host;
        if (o->ssdp_port != 0) s.ssdp_port = o->ssdp_port;
        return s;
    }
    B::ProcessSpec process_of(const SturdyProcessOptions &o) {
        B::ProcessSpec s;
        s.program = text(o.program);
        s.args = strings(o.args, o.arg_count);
        s.working_directory = text(o.working_directory);
        s.environment = fields_of(o.environment, o.environment_count);
        s.clear_environment = o.clear_environment != STURDY_FALSE;
        s.kill_on_drop = o.kill_on_release != STURDY_FALSE;
        s.stdin_mode = static_cast<B::StdioKind>(o.stdin_mode);
        s.stdout_mode = static_cast<B::StdioKind>(o.stdout_mode);
        s.stderr_mode = static_cast<B::StdioKind>(o.stderr_mode);
        return s;
    }

} // namespace SFT::Ffi::AsyncSupport

extern "C" {

// ----------------------------------------------------------------------------------------------------------------------
// Operations
// ----------------------------------------------------------------------------------------------------------------------
SturdyResult STURDY_ABI_CALL sturdy_async_features(uint32_t *out_features) {
    return run([&]() -> SturdyResult {
        if (const auto bad = require_out(out_features, "output pointer must not be null"); bad != STURDY_OK) return bad;
        *out_features = B::features();
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_op_status(SturdyAsyncOp op, SturdyOpStatus *out_status) {
    return run([&]() -> SturdyResult {
        if (const auto bad = require_out(out_status, "output pointer must not be null"); bad != STURDY_OK) return bad;
        B::OpPtr resolved;
        if (const auto r = resolve_op(op, &resolved); r != STURDY_OK) return r;
        *out_status = static_cast<SturdyOpStatus>(static_cast<int>(resolved->status()));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_op_wait(SturdyAsyncOp op, int64_t timeout_ms, SturdyBool *out_done) {
    return run([&]() -> SturdyResult {
        B::OpPtr resolved;
        if (const auto r = resolve_op(op, &resolved); r != STURDY_OK) return r;
        const bool done = resolved->wait(timeout_ms);
        if (out_done != nullptr) *out_done = done ? STURDY_TRUE : STURDY_FALSE;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_op_wait_any(const SturdyAsyncOp *ops, uint32_t count, int64_t timeout_ms, int32_t *out_index) {
    return run([&]() -> SturdyResult {
        if (ops == nullptr || count == 0) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "at least one operation is required");
        if (const auto bad = require_out(out_index, "output pointer must not be null"); bad != STURDY_OK) return bad;
        std::vector<B::OpPtr> resolved;
        for (uint32_t i = 0; i < count; ++i) {
            B::OpPtr one;
            if (const auto r = resolve_op(ops[i], &one); r != STURDY_OK) return r;
            resolved.push_back(std::move(one));
        }
        *out_index = B::wait_any(resolved, timeout_ms);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_op_cancel(SturdyAsyncOp op) {
    return run([&]() -> SturdyResult {
        B::OpPtr resolved;
        if (const auto r = resolve_op(op, &resolved); r != STURDY_OK) return r;
        resolved->cancel();
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_op_on_complete(SturdyAsyncOp op, SturdyAsyncOpFn callback, void *user_data) {
    return run([&]() -> SturdyResult {
        if (callback == nullptr) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "callback must not be null");
        B::OpPtr resolved;
        if (const auto r = resolve_op(op, &resolved); r != STURDY_OK) return r;
        const u64 token = op.token;
        resolved->on_complete([token, callback, user_data] {
            // Always delivered, even if the op was released meanwhile: the caller's `user_data` must be freeable.
            try {
                callback(user_data, SturdyAsyncOp{token});
            } catch (...) {
            }
        });
        return STURDY_OK;
    });
}

namespace {
    /// Resolves `op` and requires it to have finished.
    SturdyResult finished_op(SturdyAsyncOp handle, B::OpPtr *out, u64 *object_token = nullptr) {
        if (const auto r = resolve_op(handle, out, object_token); r != STURDY_OK) return r;
        if (!(*out)->done()) return set_error(STURDY_ERROR_BUSY, "the operation has not finished");
        return STURDY_OK;
    }
} // namespace

SturdyResult STURDY_ABI_CALL sturdy_op_error_code(SturdyAsyncOp op, SturdyIoError *out_code) {
    return run([&]() -> SturdyResult {
        if (const auto bad = require_out(out_code, "output pointer must not be null"); bad != STURDY_OK) return bad;
        B::OpPtr resolved;
        if (const auto r = finished_op(op, &resolved); r != STURDY_OK) return r;
        *out_code = resolved->result().status == B::OpStatus::Failed ? io_error_of(resolved->result().code) : STURDY_IO_NONE;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_op_error_message(SturdyAsyncOp op, char *buffer, size_t capacity, size_t *out_length) {
    return run([&]() -> SturdyResult {
        B::OpPtr resolved;
        if (const auto r = finished_op(op, &resolved); r != STURDY_OK) return r;
        return SFT::Ffi::copy_string_out(resolved->result().error, buffer, capacity, out_length);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_op_number(SturdyAsyncOp op, int64_t *out_number) {
    return run([&]() -> SturdyResult {
        if (const auto bad = require_out(out_number, "output pointer must not be null"); bad != STURDY_OK) return bad;
        B::OpPtr resolved;
        if (const auto r = finished_op(op, &resolved); r != STURDY_OK) return r;
        *out_number = resolved->result().number;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_op_text(SturdyAsyncOp op, char *buffer, size_t capacity, size_t *out_length) {
    return run([&]() -> SturdyResult {
        B::OpPtr resolved;
        if (const auto r = finished_op(op, &resolved); r != STURDY_OK) return r;
        return SFT::Ffi::copy_string_out(resolved->result().text, buffer, capacity, out_length);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_op_bytes(SturdyAsyncOp op, uint8_t *buffer, size_t capacity, size_t *out_length) {
    return run([&]() -> SturdyResult {
        B::OpPtr resolved;
        if (const auto r = finished_op(op, &resolved); r != STURDY_OK) return r;
        return copy_bytes_out(resolved->result().bytes, buffer, capacity, out_length);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_op_object(SturdyAsyncOp op, SturdyAsyncObject *out_object) {
    return run([&]() -> SturdyResult {
        if (const auto bad = require_out(out_object, "output pointer must not be null"); bad != STURDY_OK) return bad;
        B::OpPtr resolved;
        u64 existing = 0;
        if (const auto r = finished_op(op, &resolved, &existing); r != STURDY_OK) return r;
        if (existing != 0) {
            out_object->token = existing;
            return STURDY_OK;
        }
        if (!resolved->result().object) return set_error(STURDY_ERROR_NOT_AVAILABLE, "the operation produced no object");
        const u64 token = publish_object_token(resolved->result().object);
        {
            const std::lock_guard<std::mutex> lock{g_mutex};
            if (auto it = g_ops.find(op.token); it != g_ops.end()) {
                if (it->second->object_token == 0) it->second->object_token = token;
                else {
                    // Another thread asked at the same moment: keep one handle.
                    g_objects.erase(token);
                    revoke_handle(token);
                    out_object->token = it->second->object_token;
                    return STURDY_OK;
                }
            }
        }
        out_object->token = token;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_op_field_count(SturdyAsyncOp op, uint32_t *out_count) {
    return run([&]() -> SturdyResult {
        if (const auto bad = require_out(out_count, "output pointer must not be null"); bad != STURDY_OK) return bad;
        B::OpPtr resolved;
        if (const auto r = finished_op(op, &resolved); r != STURDY_OK) return r;
        *out_count = static_cast<uint32_t>(resolved->result().fields.size());
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_op_field_name(SturdyAsyncOp op, uint32_t index, char *buffer, size_t capacity, size_t *out_length) {
    return run([&]() -> SturdyResult {
        B::OpPtr resolved;
        if (const auto r = finished_op(op, &resolved); r != STURDY_OK) return r;
        const auto &fields = resolved->result().fields;
        if (index >= fields.size()) return set_error(STURDY_ERROR_OUT_OF_RANGE, "field index out of range");
        return SFT::Ffi::copy_string_out(fields[index].first, buffer, capacity, out_length);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_op_field_value(SturdyAsyncOp op, uint32_t index, char *buffer, size_t capacity, size_t *out_length) {
    return run([&]() -> SturdyResult {
        B::OpPtr resolved;
        if (const auto r = finished_op(op, &resolved); r != STURDY_OK) return r;
        const auto &fields = resolved->result().fields;
        if (index >= fields.size()) return set_error(STURDY_ERROR_OUT_OF_RANGE, "field index out of range");
        return SFT::Ffi::copy_string_out(fields[index].second, buffer, capacity, out_length);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_op_field(SturdyAsyncOp op, const char *name, char *buffer, size_t capacity, size_t *out_length) {
    return run([&]() -> SturdyResult {
        if (name == nullptr) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "name must not be null");
        B::OpPtr resolved;
        if (const auto r = finished_op(op, &resolved); r != STURDY_OK) return r;
        auto same = [](const std::string &a, const char *b) {
            const size_t n = std::strlen(b);
            if (a.size() != n) return false;
            for (size_t i = 0; i < n; ++i) {
                if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
            }
            return true;
        };
        for (const auto &[key, value] : resolved->result().fields) {
            if (key == name || same(key, name)) return SFT::Ffi::copy_string_out(value, buffer, capacity, out_length);
        }
        return set_error(STURDY_ERROR_NOT_AVAILABLE, "no such field");
    });
}

SturdyResult STURDY_ABI_CALL sturdy_op_release(SturdyAsyncOp op) {
    return run([&]() -> SturdyResult {
        void *pointer = nullptr;
        if (const auto r = resolve_handle(op.token, HandleKind::AsyncOp, &pointer); r != STURDY_OK) return r;
        revoke_handle(op.token);
        std::unique_ptr<OpEntry> dropped;
        {
            const std::lock_guard<std::mutex> lock{g_mutex};
            if (auto it = g_ops.find(op.token); it != g_ops.end()) {
                dropped = std::move(it->second);
                g_ops.erase(it);
            }
        }
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_object_release(SturdyAsyncObject object) {
    return run([&]() -> SturdyResult {
        void *pointer = nullptr;
        if (const auto r = resolve_handle(object.token, HandleKind::AsyncObject, &pointer); r != STURDY_OK) return r;
        revoke_handle(object.token);
        std::unique_ptr<B::ObjectPtr> dropped;
        {
            const std::lock_guard<std::mutex> lock{g_mutex};
            if (auto it = g_objects.find(object.token); it != g_objects.end()) {
                dropped = std::move(it->second);
                g_objects.erase(it);
            }
        }
        return STURDY_OK; // `dropped` goes out of scope here, closing the object outside the table lock
    });
}

SturdyResult STURDY_ABI_CALL sturdy_object_kind(SturdyAsyncObject object, char *buffer, size_t capacity, size_t *out_length) {
    return run([&]() -> SturdyResult {
        B::ObjectPtr resolved;
        if (const auto r = resolve_object(object, &resolved); r != STURDY_OK) return r;
        return SFT::Ffi::copy_string_out(B::object_kind(resolved), buffer, capacity, out_length);
    });
}

// ----------------------------------------------------------------------------------------------------------------------
// A shape used by almost everything below.
// ----------------------------------------------------------------------------------------------------------------------

SturdyResult STURDY_ABI_CALL sturdy_timer_sleep(int64_t milliseconds, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        return publish_op(B::sleep(milliseconds), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_signal_shutdown(SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        return publish_op(B::shutdown_signal(), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_dns_resolve(const char *host, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        if (host == nullptr) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "host must not be null");
        return publish_op(B::resolve_host(host), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_stun_query(const char *server, uint16_t port, int64_t timeout_ms, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        const std::string host = server != nullptr && *server != '\0' ? server : "stun.l.google.com";
        return publish_op(B::stun_query(host, port != 0 ? port : 19302, timeout_ms > 0 ? timeout_ms : 3000), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_tls_generate_identity(const char *common_name, uint32_t valid_days, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        if (common_name == nullptr) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "common_name must not be null");
        const B::Identity identity = B::generate_identity(common_name, valid_days != 0 ? valid_days : 365);
        return publish_fields({{"certificate_pem", identity.certificate_pem}, {"private_key_pem", identity.private_key_pem}, {"sha256", identity.sha256}}, out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_tls_certificate_sha256(const char *certificate_pem, char *buffer, size_t capacity, size_t *out_length) {
    return run([&]() -> SturdyResult {
        if (certificate_pem == nullptr) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "certificate_pem must not be null");
        return SFT::Ffi::copy_string_out(B::certificate_sha256(certificate_pem), buffer, capacity, out_length);
    });
}

// ----------------------------------------------------------------------------------------------------------------------
// TCP / UDP
// ----------------------------------------------------------------------------------------------------------------------
SturdyResult STURDY_ABI_CALL sturdy_tcp_connect(const char *host, uint16_t port, const SturdyTlsClientOptions *tls, int64_t timeout_ms, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        if (host == nullptr) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "host must not be null");
        if (tls != nullptr) {
            const B::TlsClientSpec spec = tls_of(*tls);
            return publish_op(B::tcp_connect(host, port, &spec, timeout_ms), out_op);
        }
        return publish_op(B::tcp_connect(host, port, nullptr, timeout_ms), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_tcp_listen(const char *host, uint16_t port, const SturdyTlsServerOptions *tls, int32_t backlog, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        if (tls != nullptr) {
            const B::TlsServerSpec spec = tls_of(*tls);
            return publish_op(B::tcp_listen(text(host), port, &spec, backlog), out_op);
        }
        return publish_op(B::tcp_listen(text(host), port, nullptr, backlog), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_tcp_accept(SturdyAsyncObject listener, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        STURDY_WITH_OBJECT(listener, object);
        return publish_op(B::tcp_accept(object), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_tcp_listener_port(SturdyAsyncObject listener, uint16_t *out_port) {
    return run([&]() -> SturdyResult {
        if (const auto bad = require_out(out_port, "output pointer must not be null"); bad != STURDY_OK) return bad;
        STURDY_WITH_OBJECT(listener, object);
        *out_port = B::tcp_listener_port(object);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_tcp_listener_address(SturdyAsyncObject listener, char *buffer, size_t capacity, size_t *out_length) {
    return run([&]() -> SturdyResult {
        STURDY_WITH_OBJECT(listener, object);
        return SFT::Ffi::copy_string_out(B::tcp_listener_address(object), buffer, capacity, out_length);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_tcp_listener_is_open(SturdyAsyncObject listener, SturdyBool *out_open) {
    return run([&]() -> SturdyResult {
        STURDY_REQUIRE(out_open, "output pointer must not be null");
        STURDY_WITH_OBJECT(listener, object);
        *out_open = B::tcp_listener_is_open(object) ? STURDY_TRUE : STURDY_FALSE;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_tcp_send(SturdyAsyncObject connection, const uint8_t *data, size_t size, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        STURDY_WITH_OBJECT(connection, object);
        return publish_op(B::tcp_send(object, bytes_of(data, size)), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_tcp_receive(SturdyAsyncObject connection, size_t max_bytes, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        STURDY_WITH_OBJECT(connection, object);
        return publish_op(B::tcp_receive(object, max_bytes), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_tcp_receive_exact(SturdyAsyncObject connection, size_t count, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        STURDY_WITH_OBJECT(connection, object);
        return publish_op(B::tcp_receive_exact(object, count), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_tcp_receive_until(SturdyAsyncObject connection, const char *delimiter, size_t max_bytes, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        if (delimiter == nullptr || *delimiter == '\0') return set_error(STURDY_ERROR_INVALID_ARGUMENT, "delimiter must not be empty");
        STURDY_WITH_OBJECT(connection, object);
        return publish_op(B::tcp_receive_until(object, delimiter, max_bytes), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_tcp_shutdown_write(SturdyAsyncObject connection, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        STURDY_WITH_OBJECT(connection, object);
        return publish_op(B::tcp_shutdown_write(object), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_tcp_start_tls_client(SturdyAsyncObject connection, const char *host, const SturdyTlsClientOptions *tls, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        STURDY_WITH_OBJECT(connection, object);
        const B::TlsClientSpec spec = tls != nullptr ? tls_of(*tls) : B::TlsClientSpec{};
        return publish_op(B::tcp_start_tls_client(object, text(host), spec), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_tcp_start_tls_server(SturdyAsyncObject connection, const SturdyTlsServerOptions *tls, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        if (tls == nullptr) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "TLS server options are required");
        STURDY_WITH_OBJECT(connection, object);
        return publish_op(B::tcp_start_tls_server(object, tls_of(*tls)), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_tcp_close(SturdyAsyncObject connection) {
    return run([&]() -> SturdyResult {
        STURDY_WITH_OBJECT(connection, object);
        B::tcp_close(object);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_tcp_listener_close(SturdyAsyncObject listener) {
    return run([&]() -> SturdyResult {
        STURDY_WITH_OBJECT(listener, object);
        B::tcp_listener_close(object);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_tcp_is_open(SturdyAsyncObject connection, SturdyBool *out_open) {
    return run([&]() -> SturdyResult {
        if (const auto bad = require_out(out_open, "output pointer must not be null"); bad != STURDY_OK) return bad;
        STURDY_WITH_OBJECT(connection, object);
        *out_open = B::tcp_is_open(object) ? STURDY_TRUE : STURDY_FALSE;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_tcp_set_nodelay(SturdyAsyncObject connection, SturdyBool enabled, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        STURDY_WITH_OBJECT(connection, object);
        return publish_op(B::tcp_set_nodelay(object, enabled != STURDY_FALSE), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_tcp_set_keepalive(SturdyAsyncObject connection, SturdyBool enabled, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        STURDY_WITH_OBJECT(connection, object);
        return publish_op(B::tcp_set_keepalive(object, enabled != STURDY_FALSE), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_tcp_info(SturdyAsyncObject connection, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        STURDY_WITH_OBJECT(connection, object);
        return publish_fields(B::tcp_info(object), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_udp_bind(const char *host, uint16_t port, const SturdyUdpOptions *options, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        B::UdpSpec spec;
        if (options != nullptr) {
            spec.reuse_address = options->reuse_address != STURDY_FALSE;
            spec.broadcast = options->broadcast != STURDY_FALSE;
            spec.receive_buffer = options->receive_buffer;
            spec.send_buffer = options->send_buffer;
        }
        return publish_op(B::udp_bind(text(host), port, spec), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_udp_connect(SturdyAsyncObject socket, const char *host, uint16_t port, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        if (host == nullptr) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "host must not be null");
        STURDY_WITH_OBJECT(socket, object);
        return publish_op(B::udp_connect(object, host, port), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_udp_send(SturdyAsyncObject socket, const uint8_t *data, size_t size, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        STURDY_WITH_OBJECT(socket, object);
        return publish_op(B::udp_send(object, bytes_of(data, size)), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_udp_send_to(SturdyAsyncObject socket, const char *host, uint16_t port, const uint8_t *data, size_t size, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        if (host == nullptr) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "host must not be null");
        STURDY_WITH_OBJECT(socket, object);
        return publish_op(B::udp_send_to(object, host, port, bytes_of(data, size)), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_udp_receive(SturdyAsyncObject socket, size_t max_bytes, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        STURDY_WITH_OBJECT(socket, object);
        return publish_op(B::udp_receive(object, max_bytes), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_udp_port(SturdyAsyncObject socket, uint16_t *out_port) {
    return run([&]() -> SturdyResult {
        if (const auto bad = require_out(out_port, "output pointer must not be null"); bad != STURDY_OK) return bad;
        STURDY_WITH_OBJECT(socket, object);
        *out_port = B::udp_port(object);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_udp_address(SturdyAsyncObject socket, char *buffer, size_t capacity, size_t *out_length) {
    return run([&]() -> SturdyResult {
        STURDY_WITH_OBJECT(socket, object);
        return SFT::Ffi::copy_string_out(B::udp_address(object), buffer, capacity, out_length);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_udp_set_broadcast(SturdyAsyncObject socket, SturdyBool enabled, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        STURDY_WITH_OBJECT(socket, object);
        return publish_op(B::udp_set_broadcast(object, enabled != STURDY_FALSE), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_udp_join_multicast(SturdyAsyncObject socket, const char *group, const char *interface_address, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        if (group == nullptr) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "group must not be null");
        STURDY_WITH_OBJECT(socket, object);
        return publish_op(B::udp_join_multicast(object, group, text(interface_address)), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_udp_leave_multicast(SturdyAsyncObject socket, const char *group, const char *interface_address, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        if (group == nullptr) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "group must not be null");
        STURDY_WITH_OBJECT(socket, object);
        return publish_op(B::udp_leave_multicast(object, group, text(interface_address)), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_udp_set_multicast_loopback(SturdyAsyncObject socket, SturdyBool enabled, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        STURDY_WITH_OBJECT(socket, object);
        return publish_op(B::udp_set_multicast_loopback(object, enabled != STURDY_FALSE), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_udp_set_multicast_ttl(SturdyAsyncObject socket, uint32_t hops, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        STURDY_WITH_OBJECT(socket, object);
        return publish_op(B::udp_set_multicast_ttl(object, hops), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_udp_close(SturdyAsyncObject socket) {
    return run([&]() -> SturdyResult {
        STURDY_WITH_OBJECT(socket, object);
        B::udp_close(object);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_udp_is_open(SturdyAsyncObject socket, SturdyBool *out_open) {
    return run([&]() -> SturdyResult {
        STURDY_REQUIRE(out_open, "output pointer must not be null");
        STURDY_WITH_OBJECT(socket, object);
        *out_open = B::udp_is_open(object) ? STURDY_TRUE : STURDY_FALSE;
        return STURDY_OK;
    });
}

// ----------------------------------------------------------------------------------------------------------------------
// HTTP
// ----------------------------------------------------------------------------------------------------------------------
SturdyResult STURDY_ABI_CALL sturdy_http_client_create(const SturdyHttpClientOptions *options, SturdyAsyncObject *out_client) {
    return run([&]() -> SturdyResult {
        if (const auto bad = require_out(out_client, "output pointer must not be null"); bad != STURDY_OK) return bad;
        B::HttpClientSpec spec;
        if (options != nullptr) {
            if (options->timeout_ms != 0) spec.timeout_ms = options->timeout_ms;
            if (options->max_redirects >= 0) spec.max_redirects = static_cast<uint32_t>(options->max_redirects);
            if (options->max_response_body_bytes != 0) spec.max_response_body_bytes = options->max_response_body_bytes;
            spec.keep_alive = options->no_keep_alive == STURDY_FALSE;
            spec.decompress = options->no_decompress == STURDY_FALSE;
            spec.cookies = options->cookies != STURDY_FALSE;
            spec.http2 = options->no_http2 == STURDY_FALSE;
            spec.http3 = options->no_http3 == STURDY_FALSE;
            spec.http3_prior_knowledge = options->http3_prior_knowledge != STURDY_FALSE;
            if (options->user_agent != nullptr) spec.user_agent = options->user_agent;
            spec.proxy = text(options->proxy);
            spec.no_proxy = strings(options->no_proxy, options->no_proxy_count);
            spec.tls = tls_of(options->tls);
            spec.allow_https_downgrade = options->allow_https_downgrade != STURDY_FALSE;
            if (options->max_response_header_bytes != 0) spec.max_response_header_bytes = options->max_response_header_bytes;
            if (options->max_idle_connections_per_host != 0) spec.max_idle_connections_per_host = options->max_idle_connections_per_host;
            if (options->idle_connection_timeout_ms != 0) spec.idle_connection_timeout_ms = options->idle_connection_timeout_ms;
            if (options->http3_connect_timeout_ms != 0) spec.http3_connect_timeout_ms = options->http3_connect_timeout_ms;
            if (options->cookie_jar.token != 0) {
                B::ObjectPtr jar;
                if (const auto r = resolve_object(options->cookie_jar, &jar); r != STURDY_OK) return r;
                spec.cookie_jar = std::move(jar);
            }
        } else {
            spec.max_redirects = 5;
        }
        out_client->token = publish_object_token(B::http_client_new(spec));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_http_client_request(SturdyAsyncObject client,
                                                       const char *method,
                                                       const char *url,
                                                       const SturdyKeyValue *headers,
                                                       uint32_t header_count,
                                                       const uint8_t *body,
                                                       size_t body_size,
                                                       SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        if (method == nullptr || url == nullptr) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "method and url must not be null");
        STURDY_WITH_OBJECT(client, object);
        return publish_op(B::http_request(object, method, url, fields_of(headers, header_count), bytes_of(body, body_size)), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_http_client_close_idle(SturdyAsyncObject client) {
    return run([&]() -> SturdyResult {
        STURDY_WITH_OBJECT(client, object);
        B::http_client_close_idle(object);
        return STURDY_OK;
    });
}

// ----------------------------------------------------------------------------------------------------------------------
// WebSocket
// ----------------------------------------------------------------------------------------------------------------------
SturdyResult STURDY_ABI_CALL sturdy_websocket_connect(const char *url, const SturdyWebSocketClientOptions *options, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        if (url == nullptr) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "url must not be null");
        B::WebSocketClientSpec spec;
        if (options != nullptr) {
            spec.socket = socket_of(options->socket);
            spec.tls = tls_of(options->tls);
            spec.headers = fields_of(options->headers, options->header_count);
            spec.subprotocols = strings(options->subprotocols, options->subprotocol_count);
            spec.timeout_ms = options->timeout_ms;
        }
        return publish_op(B::websocket_connect(url, spec), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_websocket_server_listen(const SturdyWebSocketServerOptions *options, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        B::WebSocketServerSpec spec;
        if (options != nullptr) {
            spec.bind_address = text(options->bind_address);
            spec.port = options->port;
            spec.socket = socket_of(options->socket);
            spec.use_tls = options->use_tls != STURDY_FALSE;
            spec.tls = tls_of(options->tls);
            spec.allowed_origins = strings(options->allowed_origins, options->allowed_origin_count);
            spec.paths = strings(options->paths, options->path_count);
            spec.subprotocols = strings(options->subprotocols, options->subprotocol_count);
            if (options->max_connections != 0) spec.max_connections = options->max_connections;
            if (options->max_request_header_bytes != 0) spec.max_request_header_bytes = options->max_request_header_bytes;
        }
        return publish_op(B::websocket_server_listen(spec), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_websocket_accept(SturdyAsyncObject server, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        STURDY_WITH_OBJECT(server, object);
        return publish_op(B::websocket_accept(object), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_websocket_server_port(SturdyAsyncObject server, uint16_t *out_port) {
    return run([&]() -> SturdyResult {
        if (const auto bad = require_out(out_port, "output pointer must not be null"); bad != STURDY_OK) return bad;
        STURDY_WITH_OBJECT(server, object);
        *out_port = B::websocket_server_port(object);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_websocket_server_close(SturdyAsyncObject server) {
    return run([&]() -> SturdyResult {
        STURDY_WITH_OBJECT(server, object);
        B::websocket_server_close(object);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_websocket_send_text(SturdyAsyncObject socket, const char *text_data, size_t size, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        if (text_data == nullptr && size != 0) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "text must not be null");
        STURDY_WITH_OBJECT(socket, object);
        return publish_op(B::websocket_send_text(object, size == 0 ? std::string{} : std::string(text_data, size)), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_websocket_send_binary(SturdyAsyncObject socket, const uint8_t *data, size_t size, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        STURDY_WITH_OBJECT(socket, object);
        return publish_op(B::websocket_send_binary(object, bytes_of(data, size)), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_websocket_ping(SturdyAsyncObject socket, const uint8_t *payload, size_t size, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        STURDY_WITH_OBJECT(socket, object);
        return publish_op(B::websocket_ping(object, bytes_of(payload, size)), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_websocket_receive(SturdyAsyncObject socket, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        STURDY_WITH_OBJECT(socket, object);
        return publish_op(B::websocket_receive(object), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_websocket_close(SturdyAsyncObject socket, uint16_t code, const char *reason, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        STURDY_WITH_OBJECT(socket, object);
        return publish_op(B::websocket_close(object, code, text(reason)), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_websocket_is_open(SturdyAsyncObject socket, SturdyBool *out_open) {
    return run([&]() -> SturdyResult {
        if (const auto bad = require_out(out_open, "output pointer must not be null"); bad != STURDY_OK) return bad;
        STURDY_WITH_OBJECT(socket, object);
        *out_open = B::websocket_is_open(object) ? STURDY_TRUE : STURDY_FALSE;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_websocket_abort(SturdyAsyncObject socket) {
    return run([&]() -> SturdyResult {
        STURDY_WITH_OBJECT(socket, object);
        B::websocket_abort(object);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_websocket_info(SturdyAsyncObject socket, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        STURDY_WITH_OBJECT(socket, object);
        return publish_fields(B::websocket_info(object), out_op);
    });
}

// ----------------------------------------------------------------------------------------------------------------------
// QUIC
// ----------------------------------------------------------------------------------------------------------------------
SturdyResult STURDY_ABI_CALL sturdy_quic_connect(const char *host, uint16_t port, const SturdyQuicClientOptions *options, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        if (host == nullptr) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "host must not be null");
        B::QuicClientSpec spec;
        if (options != nullptr) {
            spec.limits = limits_of(options->limits);
            spec.tls = tls_of(options->tls);
        }
        return publish_op(B::quic_connect(host, port, spec), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_quic_listen(const SturdyQuicServerOptions *options, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        if (options == nullptr) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "QUIC server options are required (certificate, key, ALPN)");
        B::QuicServerSpec spec;
        spec.limits = limits_of(options->limits);
        spec.tls = tls_of(options->tls);
        spec.bind_address = text(options->bind_address);
        spec.port = options->port;
        if (options->max_connections != 0) spec.max_connections = options->max_connections;
        if (options->max_connections_per_address != 0) spec.max_connections_per_address = options->max_connections_per_address;
        spec.require_address_validation = options->no_address_validation == STURDY_FALSE;
        return publish_op(B::quic_listen(spec), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_quic_accept(SturdyAsyncObject listener, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        STURDY_WITH_OBJECT(listener, object);
        return publish_op(B::quic_accept(object), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_quic_listener_port(SturdyAsyncObject listener, uint16_t *out_port) {
    return run([&]() -> SturdyResult {
        if (const auto bad = require_out(out_port, "output pointer must not be null"); bad != STURDY_OK) return bad;
        STURDY_WITH_OBJECT(listener, object);
        *out_port = B::quic_listener_port(object);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_quic_listener_metrics(SturdyAsyncObject listener, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        STURDY_WITH_OBJECT(listener, object);
        return publish_fields(B::quic_listener_metrics(object), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_quic_listener_stop(SturdyAsyncObject listener) {
    return run([&]() -> SturdyResult {
        STURDY_WITH_OBJECT(listener, object);
        B::quic_listener_stop(object);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_quic_open_stream(SturdyAsyncObject connection, SturdyBool bidirectional, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        STURDY_WITH_OBJECT(connection, object);
        return publish_op(B::quic_open_stream(object, bidirectional != STURDY_FALSE), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_quic_accept_stream(SturdyAsyncObject connection, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        STURDY_WITH_OBJECT(connection, object);
        return publish_op(B::quic_accept_stream(object), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_quic_send_datagram(SturdyAsyncObject connection, const uint8_t *data, size_t size) {
    return guarded([&]() -> SturdyResult {
        STURDY_WITH_OBJECT(connection, object);
        try {
            B::quic_send_datagram(object, bytes_of(data, size));
        } catch (const B::BindingError &error) {
            // Distinguish the three ways a datagram can be refused so the caller can react differently to each.
            const std::string message = error.what();
            if (error.code() == SFT::Async::IoErrorCode::Unsupported) return set_error(STURDY_ERROR_NOT_AVAILABLE, message);
            if (message.rfind("datagram larger", 0) == 0) return set_error(STURDY_ERROR_OUT_OF_RANGE, message);
            if (error.code() == SFT::Async::IoErrorCode::LimitExceeded) return set_error(STURDY_ERROR_BUSY, message);
            return set_error(map_code(error.code()), message);
        }
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_quic_receive_datagram(SturdyAsyncObject connection, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        STURDY_WITH_OBJECT(connection, object);
        return publish_op(B::quic_receive_datagram(object), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_quic_max_datagram_size(SturdyAsyncObject connection, size_t *out_size) {
    return run([&]() -> SturdyResult {
        if (const auto bad = require_out(out_size, "output pointer must not be null"); bad != STURDY_OK) return bad;
        STURDY_WITH_OBJECT(connection, object);
        *out_size = B::quic_max_datagram_size(object);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_quic_close(SturdyAsyncObject connection, uint64_t error_code, const char *reason) {
    return run([&]() -> SturdyResult {
        STURDY_WITH_OBJECT(connection, object);
        B::quic_close(object, error_code, text(reason));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_quic_closed(SturdyAsyncObject connection, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        STURDY_WITH_OBJECT(connection, object);
        return publish_op(B::quic_closed(object), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_quic_is_open(SturdyAsyncObject connection, SturdyBool *out_open) {
    return run([&]() -> SturdyResult {
        if (const auto bad = require_out(out_open, "output pointer must not be null"); bad != STURDY_OK) return bad;
        STURDY_WITH_OBJECT(connection, object);
        *out_open = B::quic_is_open(object) ? STURDY_TRUE : STURDY_FALSE;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_quic_info(SturdyAsyncObject connection, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        STURDY_WITH_OBJECT(connection, object);
        return publish_fields(B::quic_info(object), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_quic_stats(SturdyAsyncObject connection, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        STURDY_WITH_OBJECT(connection, object);
        return publish_fields(B::quic_stats(object), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_quic_stream_send(SturdyAsyncObject stream, const uint8_t *data, size_t size, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        STURDY_WITH_OBJECT(stream, object);
        return publish_op(B::quic_stream_send(object, bytes_of(data, size)), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_quic_stream_finish(SturdyAsyncObject stream, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        STURDY_WITH_OBJECT(stream, object);
        return publish_op(B::quic_stream_finish(object), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_quic_stream_receive(SturdyAsyncObject stream, size_t max_bytes, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        STURDY_WITH_OBJECT(stream, object);
        return publish_op(B::quic_stream_receive(object, max_bytes), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_quic_stream_reset(SturdyAsyncObject stream, uint64_t error_code) {
    return run([&]() -> SturdyResult {
        STURDY_WITH_OBJECT(stream, object);
        B::quic_stream_reset(object, error_code);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_quic_stream_stop_sending(SturdyAsyncObject stream, uint64_t error_code) {
    return run([&]() -> SturdyResult {
        STURDY_WITH_OBJECT(stream, object);
        B::quic_stream_stop_sending(object, error_code);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_quic_stream_id(SturdyAsyncObject stream, int64_t *out_id) {
    return run([&]() -> SturdyResult {
        if (const auto bad = require_out(out_id, "output pointer must not be null"); bad != STURDY_OK) return bad;
        STURDY_WITH_OBJECT(stream, object);
        *out_id = B::quic_stream_id(object);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_quic_stream_is_bidirectional(SturdyAsyncObject stream, SturdyBool *out_bidirectional) {
    return run([&]() -> SturdyResult {
        if (const auto bad = require_out(out_bidirectional, "output pointer must not be null"); bad != STURDY_OK) return bad;
        STURDY_WITH_OBJECT(stream, object);
        *out_bidirectional = B::quic_stream_is_bidirectional(object) ? STURDY_TRUE : STURDY_FALSE;
        return STURDY_OK;
    });
}

// ----------------------------------------------------------------------------------------------------------------------
// Peers, relay, NAT
// ----------------------------------------------------------------------------------------------------------------------
SturdyResult STURDY_ABI_CALL sturdy_peer_start(const SturdyPeerOptions *options, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        if (options == nullptr || options->network_id == nullptr) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "a network id is required");
        B::PeerSpec spec;
        spec.network_id = options->network_id;
        spec.display_name = text(options->display_name);
        if (options->identity_certificate_pem != nullptr || options->identity_private_key_pem != nullptr) {
            if (options->identity_certificate_pem == nullptr || options->identity_private_key_pem == nullptr) {
                return set_error(STURDY_ERROR_INVALID_ARGUMENT, "identity certificate and key must be given together");
            }
            spec.has_identity = true;
            spec.identity.certificate_pem = options->identity_certificate_pem;
            spec.identity.private_key_pem = options->identity_private_key_pem;
        }
        spec.listen = options->no_listen == STURDY_FALSE;
        spec.bind_address = text(options->bind_address);
        spec.port = options->port;
        if (options->max_peers != 0) spec.max_peers = options->max_peers;
        if (options->max_message_bytes != 0) spec.max_message_bytes = options->max_message_bytes;
        spec.allowed_peers = strings(options->allowed_peers, options->allowed_peer_count);
        spec.peer_exchange = options->no_peer_exchange == STURDY_FALSE;
        spec.lan_discovery = options->lan_discovery != STURDY_FALSE;
        spec.auto_connect = options->no_auto_connect == STURDY_FALSE;
        if (options->handshake_timeout_ms != 0) spec.handshake_timeout_ms = options->handshake_timeout_ms;
        if (options->max_pending_inbound != 0) spec.max_pending_inbound = options->max_pending_inbound;
        if (options->ping_interval_ms != 0) spec.ping_interval_ms = options->ping_interval_ms;
        if (options->idle_timeout_ms != 0) spec.idle_timeout_ms = options->idle_timeout_ms;
        if (options->event_queue != 0) spec.event_queue = options->event_queue;
        if (options->discovery_port != 0) spec.discovery_port = options->discovery_port;
        if (options->discovery_group != nullptr) spec.discovery_group = options->discovery_group;
        if (options->discovery_interval_ms != 0) spec.discovery_interval_ms = options->discovery_interval_ms;
        return publish_op(B::peer_start(spec), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_peer_id(SturdyAsyncObject peer, char *buffer, size_t capacity, size_t *out_length) {
    return run([&]() -> SturdyResult {
        STURDY_WITH_OBJECT(peer, object);
        return SFT::Ffi::copy_string_out(B::peer_id(object), buffer, capacity, out_length);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_peer_listen_port(SturdyAsyncObject peer, uint16_t *out_port) {
    return run([&]() -> SturdyResult {
        if (const auto bad = require_out(out_port, "output pointer must not be null"); bad != STURDY_OK) return bad;
        STURDY_WITH_OBJECT(peer, object);
        *out_port = B::peer_listen_port(object);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_peer_identity(SturdyAsyncObject peer, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        STURDY_WITH_OBJECT(peer, object);
        const B::Identity identity = B::peer_identity(object);
        return publish_fields({{"certificate_pem", identity.certificate_pem}, {"private_key_pem", identity.private_key_pem}, {"sha256", identity.sha256}}, out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_peer_connect(SturdyAsyncObject peer, const char *host, uint16_t port, const char *expected_id, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        if (host == nullptr) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "host must not be null");
        STURDY_WITH_OBJECT(peer, object);
        return publish_op(B::peer_connect(object, host, port, text(expected_id)), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_peer_connect_via_relay(SturdyAsyncObject peer, const char *relay_host, uint16_t relay_port, const char *token, SturdyBool dial, const char *expected_id, const SturdyTlsClientOptions *relay_tls, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        if (relay_host == nullptr || token == nullptr) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "relay host and token must not be null");
        STURDY_WITH_OBJECT(peer, object);
        const B::TlsClientSpec tls = relay_tls != nullptr ? tls_of(*relay_tls) : B::TlsClientSpec{};
        return publish_op(B::peer_connect_via_relay(object, relay_host, relay_port, token, dial != STURDY_FALSE, text(expected_id), relay_tls != nullptr ? &tls : nullptr), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_peer_send(SturdyAsyncObject peer, const char *peer_id, uint16_t channel, const uint8_t *data, size_t size, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        if (peer_id == nullptr) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "peer_id must not be null");
        STURDY_WITH_OBJECT(peer, object);
        return publish_op(B::peer_send(object, peer_id, channel, bytes_of(data, size)), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_peer_broadcast(SturdyAsyncObject peer, uint16_t channel, const uint8_t *data, size_t size, const char *except_peer_id, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        STURDY_WITH_OBJECT(peer, object);
        return publish_op(B::peer_broadcast(object, channel, bytes_of(data, size), text(except_peer_id)), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_peer_next_event(SturdyAsyncObject peer, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        STURDY_WITH_OBJECT(peer, object);
        return publish_op(B::peer_next_event(object), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_peer_list(SturdyAsyncObject peer, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        STURDY_WITH_OBJECT(peer, object);
        return publish_fields(B::peer_list(object), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_peer_disconnect(SturdyAsyncObject peer, const char *peer_id, const char *reason) {
    return run([&]() -> SturdyResult {
        if (peer_id == nullptr) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "peer_id must not be null");
        STURDY_WITH_OBJECT(peer, object);
        B::peer_disconnect(object, peer_id, text(reason));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_peer_block(SturdyAsyncObject peer, const char *peer_id) {
    return run([&]() -> SturdyResult {
        if (peer_id == nullptr) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "peer_id must not be null");
        STURDY_WITH_OBJECT(peer, object);
        B::peer_block(object, peer_id);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_peer_stop(SturdyAsyncObject peer) {
    return run([&]() -> SturdyResult {
        STURDY_WITH_OBJECT(peer, object);
        B::peer_stop(object);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_relay_server_start(const SturdyRelayOptions *options, SturdyRelayAuthorizeFn authorize, void *user_data, SturdyAsyncObject *out_relay) {
    return run([&]() -> SturdyResult {
        if (const auto bad = require_out(out_relay, "output pointer must not be null"); bad != STURDY_OK) return bad;
        B::RelaySpec spec;
        if (options != nullptr) {
            spec.bind_address = text(options->bind_address);
            spec.port = options->port;
            spec.use_tls = options->use_tls != STURDY_FALSE;
            spec.tls = tls_of(options->tls);
            if (options->max_sessions != 0) spec.max_sessions = options->max_sessions;
            if (options->max_waiting != 0) spec.max_waiting = options->max_waiting;
            if (options->max_waiting_per_address != 0) spec.max_waiting_per_address = options->max_waiting_per_address;
            if (options->pair_timeout_ms != 0) spec.pair_timeout_ms = options->pair_timeout_ms;
            if (options->idle_timeout_ms != 0) spec.idle_timeout_ms = options->idle_timeout_ms;
            spec.max_session_bytes = options->max_session_bytes;
            if (options->max_token_bytes != 0) spec.max_token_bytes = options->max_token_bytes;
        }
        B::RelayAuthorizeFn check;
        if (authorize != nullptr) {
            check = [authorize, user_data](const std::string &token, const std::string &client) { return authorize(user_data, token.c_str(), client.c_str()) != STURDY_FALSE; };
        }
        out_relay->token = publish_object_token(B::relay_server_start(spec, std::move(check)));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_relay_server_port(SturdyAsyncObject relay, uint16_t *out_port) {
    return run([&]() -> SturdyResult {
        if (const auto bad = require_out(out_port, "output pointer must not be null"); bad != STURDY_OK) return bad;
        STURDY_WITH_OBJECT(relay, object);
        *out_port = B::relay_server_port(object);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_relay_server_metrics(SturdyAsyncObject relay, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        STURDY_WITH_OBJECT(relay, object);
        return publish_fields(B::relay_server_metrics(object), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_relay_server_stop(SturdyAsyncObject relay) {
    return run([&]() -> SturdyResult {
        STURDY_WITH_OBJECT(relay, object);
        B::relay_server_stop(object);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_relay_random_token(char *buffer, size_t capacity, size_t *out_length) {
    return run([&]() -> SturdyResult { return SFT::Ffi::copy_string_out(B::random_relay_token(), buffer, capacity, out_length); });
}

SturdyResult STURDY_ABI_CALL sturdy_relay_connect(const char *host, uint16_t port, const char *token, SturdyBool dial, const SturdyTlsClientOptions *relay_tls, int64_t timeout_ms, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        if (host == nullptr || token == nullptr) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "host and token must not be null");
        const std::int64_t timeout = timeout_ms > 0 ? timeout_ms : 35'000;
        if (relay_tls != nullptr) {
            const B::TlsClientSpec spec = tls_of(*relay_tls);
            return publish_op(B::relay_connect(host, port, token, dial != STURDY_FALSE, &spec, timeout), out_op);
        }
        return publish_op(B::relay_connect(host, port, token, dial != STURDY_FALSE, nullptr, timeout), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_nat_map_port(SturdyBool tcp, uint16_t internal_port, const SturdyPortMapOptions *options, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        return publish_op(B::map_port(tcp != STURDY_FALSE, internal_port, portmap_of(options)), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_nat_unmap_port(SturdyAsyncObject mapping, const SturdyPortMapOptions *options, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        STURDY_WITH_OBJECT(mapping, object);
        return publish_op(B::unmap_port(object, portmap_of(options)), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_nat_default_gateway(char *buffer, size_t capacity, size_t *out_length) {
    return run([&]() -> SturdyResult { return SFT::Ffi::copy_string_out(B::default_gateway(), buffer, capacity, out_length); });
}

SturdyResult STURDY_ABI_CALL sturdy_nat_punch_hole(SturdyAsyncObject udp_socket, const char *const *candidates, uint32_t candidate_count, const char *shared_secret, int64_t timeout_ms, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        if (candidates == nullptr || candidate_count == 0 || shared_secret == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "candidates and a shared secret are required");
        }
        std::vector<std::pair<std::string, std::uint16_t>> parsed;
        for (uint32_t i = 0; i < candidate_count; ++i) {
            const std::string entry = text(candidates[i]);
            const size_t colon = entry.rfind(':');
            if (colon == std::string::npos || colon == 0) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "candidates are written host:port");
            const long port = std::strtol(entry.c_str() + colon + 1, nullptr, 10);
            if (port <= 0 || port > 65535) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "candidate port out of range");
            parsed.emplace_back(entry.substr(0, colon), static_cast<std::uint16_t>(port));
        }
        STURDY_WITH_OBJECT(udp_socket, object);
        return publish_op(B::punch_udp_hole(object, parsed, shared_secret, timeout_ms > 0 ? timeout_ms : 8000), out_op);
    });
}

// ----------------------------------------------------------------------------------------------------------------------
// Files, watching, processes
// ----------------------------------------------------------------------------------------------------------------------
SturdyResult STURDY_ABI_CALL sturdy_file_open(const char *path, uint32_t mode, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        if (path == nullptr) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "path must not be null");
        return publish_op(B::file_open(path, mode), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_file_read(SturdyAsyncObject file, size_t max_bytes, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        STURDY_WITH_OBJECT(file, object);
        return publish_op(B::file_read(object, max_bytes), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_file_read_at(SturdyAsyncObject file, uint64_t offset, size_t max_bytes, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        STURDY_WITH_OBJECT(file, object);
        return publish_op(B::file_read_at(object, offset, max_bytes), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_file_read_to_end(SturdyAsyncObject file, size_t max_bytes, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        STURDY_WITH_OBJECT(file, object);
        return publish_op(B::file_read_to_end(object, max_bytes), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_file_write(SturdyAsyncObject file, const uint8_t *data, size_t size, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        STURDY_WITH_OBJECT(file, object);
        return publish_op(B::file_write(object, bytes_of(data, size)), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_file_write_at(SturdyAsyncObject file, uint64_t offset, const uint8_t *data, size_t size, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        STURDY_WITH_OBJECT(file, object);
        return publish_op(B::file_write_at(object, offset, bytes_of(data, size)), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_file_seek(SturdyAsyncObject file, int64_t offset, int32_t origin, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        if (origin < 0 || origin > 2) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "origin must be 0, 1 or 2");
        STURDY_WITH_OBJECT(file, object);
        return publish_op(B::file_seek(object, offset, origin), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_file_size(SturdyAsyncObject file, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        STURDY_WITH_OBJECT(file, object);
        return publish_op(B::file_size(object), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_file_set_len(SturdyAsyncObject file, uint64_t length, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        STURDY_WITH_OBJECT(file, object);
        return publish_op(B::file_set_len(object, length), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_file_sync(SturdyAsyncObject file, SturdyBool data_only, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        STURDY_WITH_OBJECT(file, object);
        return publish_op(B::file_sync(object, data_only != STURDY_FALSE), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_file_close(SturdyAsyncObject file) {
    return run([&]() -> SturdyResult {
        STURDY_WITH_OBJECT(file, object);
        B::file_close(object);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_watch_path(const char *path, const SturdyWatchOptions *options, SturdyAsyncObject *out_watcher) {
    return run([&]() -> SturdyResult {
        if (const auto bad = require_out(out_watcher, "output pointer must not be null"); bad != STURDY_OK) return bad;
        if (path == nullptr) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "path must not be null");
        B::WatchSpec spec;
        if (options != nullptr) {
            spec.recursive = options->recursive != STURDY_FALSE;
            if (options->debounce_ms > 0) spec.debounce_ms = options->debounce_ms;
            spec.force_polling = options->force_polling != STURDY_FALSE;
            if (options->poll_interval_ms > 0) spec.poll_interval_ms = options->poll_interval_ms;
            if (options->max_pending_events != 0) spec.max_pending_events = options->max_pending_events;
        }
        out_watcher->token = publish_object_token(B::watch_path(path, spec));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_watch_uses_native_backend(SturdyAsyncObject watcher, SturdyBool *out_native) {
    return run([&]() -> SturdyResult {
        STURDY_REQUIRE(out_native, "output pointer must not be null");
        STURDY_WITH_OBJECT(watcher, object);
        *out_native = B::watch_uses_native_backend(object) ? STURDY_TRUE : STURDY_FALSE;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_watch_events_stream(SturdyAsyncObject watcher, SturdyAsyncObject *out_stream) {
    return run([&]() -> SturdyResult {
        STURDY_REQUIRE(out_stream, "output pointer must not be null");
        STURDY_WITH_OBJECT(watcher, object);
        *out_stream = publish_object(B::watch_events_stream(object));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_watch_next(SturdyAsyncObject watcher, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        STURDY_WITH_OBJECT(watcher, object);
        return publish_op(B::watch_next(object), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_watch_close(SturdyAsyncObject watcher) {
    return run([&]() -> SturdyResult {
        STURDY_WITH_OBJECT(watcher, object);
        B::watch_close(object);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_process_run(const SturdyProcessOptions *options, const uint8_t *stdin_data, size_t stdin_size, int64_t timeout_ms, uint64_t max_output_bytes, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        if (options == nullptr || options->program == nullptr) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "a program is required");
        return publish_op(B::process_run(process_of(*options), bytes_of(stdin_data, stdin_size), timeout_ms, max_output_bytes), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_process_spawn(const SturdyProcessOptions *options, SturdyAsyncObject *out_process) {
    return run([&]() -> SturdyResult {
        if (const auto bad = require_out(out_process, "output pointer must not be null"); bad != STURDY_OK) return bad;
        if (options == nullptr || options->program == nullptr) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "a program is required");
        out_process->token = publish_object_token(B::process_spawn(process_of(*options)));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_process_write_stdin(SturdyAsyncObject process, const uint8_t *data, size_t size, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        STURDY_WITH_OBJECT(process, object);
        return publish_op(B::process_write_stdin(object, bytes_of(data, size)), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_process_close_stdin(SturdyAsyncObject process) {
    return run([&]() -> SturdyResult {
        STURDY_WITH_OBJECT(process, object);
        B::process_close_stdin(object);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_process_read_stdout(SturdyAsyncObject process, size_t max_bytes, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        STURDY_WITH_OBJECT(process, object);
        return publish_op(B::process_read_stdout(object, max_bytes), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_process_read_stderr(SturdyAsyncObject process, size_t max_bytes, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        STURDY_WITH_OBJECT(process, object);
        return publish_op(B::process_read_stderr(object, max_bytes), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_process_wait(SturdyAsyncObject process, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        STURDY_WITH_OBJECT(process, object);
        return publish_op(B::process_wait(object), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_process_id(SturdyAsyncObject process, int64_t *out_id) {
    return run([&]() -> SturdyResult {
        STURDY_REQUIRE(out_id, "output pointer must not be null");
        STURDY_WITH_OBJECT(process, object);
        *out_id = B::process_id(object);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_process_request_exit(SturdyAsyncObject process) {
    return run([&]() -> SturdyResult {
        STURDY_WITH_OBJECT(process, object);
        B::process_request_exit(object);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_process_kill(SturdyAsyncObject process) {
    return run([&]() -> SturdyResult {
        STURDY_WITH_OBJECT(process, object);
        B::process_kill(object);
        return STURDY_OK;
    });
}

// ----------------------------------------------------------------------------------------------------------------------
// Data parallelism
// ----------------------------------------------------------------------------------------------------------------------
SturdyResult STURDY_ABI_CALL sturdy_par_for(uint64_t count, uint64_t min_chunk, SturdyParForFn body, void *user_data) {
    return run([&]() -> SturdyResult {
        if (body == nullptr) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "body must not be null");
        B::par_for(count, min_chunk, [body, user_data](uint64_t index) { body(user_data, index); });
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_par_for_chunks(uint64_t count, uint64_t chunk, SturdyParChunkFn body, void *user_data) {
    return run([&]() -> SturdyResult {
        if (body == nullptr) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "body must not be null");
        if (chunk == 0) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "chunk must be positive");
        B::par_for_chunks(count, chunk, [body, user_data](uint64_t begin, uint64_t end) { body(user_data, begin, end); });
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_par_sort(SturdyNumericType type, void *data, uint64_t count) {
    return run([&]() -> SturdyResult {
        if (data == nullptr && count != 0) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "data must not be null");
        if (type < STURDY_NUMERIC_F32 || type > STURDY_NUMERIC_U64) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "unknown numeric type");
        B::par_sort(static_cast<B::Numeric>(type), data, count);
        return STURDY_OK;
    });
}

#define STURDY_PAR_SUM(name, type, call)                                                                                                  \
    SturdyResult STURDY_ABI_CALL name(const type *data, uint64_t count, type *out_sum) {                                                  \
        return run([&]() -> SturdyResult {                                                                                                \
            if (out_sum == nullptr || (data == nullptr && count != 0)) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "null pointer");  \
            *out_sum = call(data, count);                                                                                                 \
            return STURDY_OK;                                                                                                             \
        });                                                                                                                               \
    }
STURDY_PAR_SUM(sturdy_par_sum_f32, float, B::par_sum_f32)
STURDY_PAR_SUM(sturdy_par_sum_f64, double, B::par_sum_f64)
STURDY_PAR_SUM(sturdy_par_sum_i64, int64_t, B::par_sum_i64)
STURDY_PAR_SUM(sturdy_par_sum_u64, uint64_t, B::par_sum_u64)

#define STURDY_PAR_MIN_MAX(name, type, call)                                                                                              \
    SturdyResult STURDY_ABI_CALL name(const type *data, uint64_t count, type *out_min, type *out_max) {                                   \
        return run([&]() -> SturdyResult {                                                                                                \
            if (out_min == nullptr || out_max == nullptr || data == nullptr) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "null pointer"); \
            if (!call(data, count, out_min, out_max)) return set_error(STURDY_ERROR_OUT_OF_RANGE, "there are no elements");               \
            return STURDY_OK;                                                                                                             \
        });                                                                                                                               \
    }
STURDY_PAR_MIN_MAX(sturdy_par_min_max_f32, float, B::par_min_max_f32)
STURDY_PAR_MIN_MAX(sturdy_par_min_max_f64, double, B::par_min_max_f64)
STURDY_PAR_MIN_MAX(sturdy_par_min_max_i64, int64_t, B::par_min_max_i64)

SturdyResult STURDY_ABI_CALL sturdy_pool_create(uint32_t threads, const char *name, SturdyAsyncObject *out_pool) {
    return run([&]() -> SturdyResult {
        if (const auto bad = require_out(out_pool, "output pointer must not be null"); bad != STURDY_OK) return bad;
        out_pool->token = publish_object_token(B::pool_create(threads, text(name)));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_pool_threads(SturdyAsyncObject pool, uint32_t *out_threads) {
    return run([&]() -> SturdyResult {
        if (const auto bad = require_out(out_threads, "output pointer must not be null"); bad != STURDY_OK) return bad;
        STURDY_WITH_OBJECT(pool, object);
        *out_threads = B::pool_threads(object);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_pool_par_for(SturdyAsyncObject pool, uint64_t count, uint64_t min_chunk, SturdyParForFn body, void *user_data) {
    return run([&]() -> SturdyResult {
        if (body == nullptr) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "body must not be null");
        STURDY_WITH_OBJECT(pool, object);
        B::pool_par_for(object, count, min_chunk, [body, user_data](uint64_t index) { body(user_data, index); });
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_pool_run(SturdyAsyncObject pool, SturdyTaskFn task, void *user_data) {
    return run([&]() -> SturdyResult {
        if (task == nullptr) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "task must not be null");
        STURDY_WITH_OBJECT(pool, object);
        B::pool_install(object, [task, user_data] { task(user_data); });
        return STURDY_OK;
    });
}

} // extern "C"
