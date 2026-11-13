#pragma once

// Boost.Asio's TLS support, named header by header instead of through <boost/asio/ssl.hpp>.
//
// The umbrella header also pulls in `rfc2818_verification.hpp`, whose implementation reads the members of OpenSSL's
// ASN1_STRING and X509_NAME directly. OpenSSL 4 made those types opaque, so with it installed (Arch ships 4.1) every
// translation file that included the umbrella failed to compile. Nothing here verifies host names through that class
// (a client that wants a host checked uses `host_name_verification` or OpenSSL's own `SSL_set1_host`), so it is left out.
// Everything else the umbrella provides is here.

#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/context_base.hpp>
#include <boost/asio/ssl/error.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/asio/ssl/stream_base.hpp>
#include <boost/asio/ssl/verify_context.hpp>
#include <boost/asio/ssl/verify_mode.hpp>
