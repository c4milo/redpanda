#include "server.h"

#include "kafka/requests/request_context.h"
#include "kafka/requests/response.h"
#include "prometheus/prometheus_sanitize.h"
#include "utils/utf8.h"

#include <seastar/core/byteorder.hh>
#include <seastar/core/metrics.hh>
#include <seastar/core/scattered_message.hh>
#include <seastar/core/sleep.hh>
#include <seastar/net/api.hh>
#include <seastar/util/log.hh>

#include <fmt/format.h>

namespace kafka {

static ss::logger klog("kafka_server");

kafka_server::kafka_server(
  probe p,
  ss::sharded<cluster::metadata_cache>& metadata_cache,
  ss::sharded<controller_dispatcher>& cntrl_dispatcher,
  kafka_server_config config,
  ss::sharded<quota_manager>& quota_mgr,
  ss::sharded<group_router_type>& group_router,
  ss::sharded<cluster::shard_table>& shard_table,
  ss::sharded<cluster::partition_manager>& partition_manager) noexcept
  : _probe(std::move(p))
  , _metadata_cache(metadata_cache)
  , _cntrl_dispatcher(cntrl_dispatcher)
  , _max_request_size(config.max_request_size)
  , _memory_available(_max_request_size)
  , _smp_group(std::move(config.smp_group))
  , _quota_mgr(quota_mgr)
  , _group_router(group_router)
  , _shard_table(shard_table)
  , _partition_manager(partition_manager)
  , _creds(
      config.credentials ? (*config.credentials).build_server_credentials()
                         : nullptr) {
    _probe.setup_metrics(_metrics);
}

ss::future<>
kafka_server::listen(ss::socket_address server_addr, bool keepalive) {
    ss::listen_options lo;
    lo.reuse_address = true;
    ss::server_socket ss;
    try {
        if (!_creds) {
            ss = ss::engine().listen(server_addr, lo);
            klog.debug(
              "Started plaintext Kafka API server listening at {}",
              server_addr);
        } else {
            ss = ss::tls::listen(_creds, ss::engine().listen(server_addr, lo));
            klog.debug(
              "Started secured Kafka API server listening at {}", server_addr);
        }
    } catch (...) {
        return ss::make_exception_future<>(std::runtime_error(fmt::format(
          "KafkaServer error while listening on {} -> {}",
          server_addr,
          std::current_exception())));
    }
    _listeners.emplace_back(std::move(ss));

    (void)with_gate(_listeners_and_connections, [this, keepalive, server_addr] {
        return do_accepts(_listeners.size() - 1, keepalive);
    });
    return ss::make_ready_future<>();
}

ss::future<> kafka_server::do_accepts(int which, bool keepalive) {
    return ss::repeat([this, which, keepalive] {
        return _listeners[which]
          .accept()
          .then_wrapped([this, which, keepalive](
                          ss::future<ss::accept_result> f_ar) mutable {
              if (_as.abort_requested()) {
                  f_ar.ignore_ready_future();
                  return ss::stop_iteration::yes;
              }
              auto [fd, addr] = f_ar.get0();
              fd.set_nodelay(true);
              fd.set_keepalive(keepalive);
              auto conn = std::make_unique<connection>(
                *this, std::move(fd), std::move(addr));
              (void)with_gate(
                _listeners_and_connections,
                [this, conn = std::move(conn)]() mutable {
                    auto f = conn->process();
                    return f.then_wrapped([conn = std::move(conn)](
                                            ss::future<>&& f) {
                        try {
                            f.get();
                        } catch (...) {
                            klog.debug(
                              "Connection error: {}", std::current_exception());
                        }
                    });
                });
              return ss::stop_iteration::no;
          })
          .handle_exception([](std::exception_ptr ep) {
              klog.debug("Accept failed: {}", ep);
              return ss::stop_iteration::no;
          });
    });
}

ss::future<> kafka_server::stop() {
    klog.debug("Aborting {} listeners", _listeners.size());
    for (auto&& l : _listeners) {
        l.abort_accept();
    }
    klog.debug("Shutting down {} connections", _connections.size());
    _as.request_abort();
    for (auto&& con : _connections) {
        con.shutdown();
    }
    return _listeners_and_connections.close();
}

kafka_server::connection::connection(
  kafka_server& server, ss::connected_socket&& fd, ss::socket_address addr)
  : _server(server)
  , _fd(std::move(fd))
  , _addr(std::move(addr))
  , _read_buf(_fd.input())
  , _write_buf(_fd.output()) {
    _server._probe.connection_established();
    _server._connections.push_back(*this);
}

kafka_server::connection::~connection() {
    _server._probe.connection_closed();
    _server._connections.erase(_server._connections.iterator_to(*this));
}

void kafka_server::connection::shutdown() {
    try {
        _fd.shutdown_input();
        _fd.shutdown_output();
    } catch (...) {
        klog.debug(
          "Failed to shutdown conneciton: {}", std::current_exception());
    }
}

// clang-format off
ss::future<> kafka_server::connection::process() {
    return ss::do_until(
      [this] {
          return _read_buf.eof() || _server._as.abort_requested();
      },
      [this] {
          return process_request().handle_exception([this](std::exception_ptr e) {
              klog.error("Failed to process request with {}", e);
          });
      })
      .then([this] {
          return _ready_to_respond.then([this] {
              return _write_buf.close();
          });
      });
}
// clang-format on

ss::future<> kafka_server::connection::write_response(
  response_ptr&& response, correlation_type correlation_id) {
    ss::sstring header(
      ss::sstring::initialized_later(), sizeof(raw_response_header));
    auto* raw_header = reinterpret_cast<raw_response_header*>(header.begin());
    auto size = size_type(
      sizeof(correlation_type) + response->buf().size_bytes());
    raw_header->size = ss::cpu_to_be(size);
    raw_header->correlation_id = ss::cpu_to_be(correlation_id);

    ss::scattered_message<char> msg;
    msg.append(std::move(header));
    for (auto&& chunk : response->buf()) {
        msg.append_static(
          reinterpret_cast<const char*>(chunk.get()), chunk.size());
    }
    msg.on_delete([response = std::move(response)] {});
    auto msg_size = msg.size();
    return _write_buf.write(std::move(msg))
      .then([this] { return _write_buf.flush(); })
      .then([this, msg_size] { _server._probe.add_bytes_sent(msg_size); });
}

// The server guarantees that on a single TCP connection, requests will be
// processed in the order they are sent and responses will return in that order
// as well.
ss::future<> kafka_server::connection::process_request() {
    return _read_buf.read_exactly(sizeof(size_type))
      .then([this](ss::temporary_buffer<char> buf) {
          if (!buf) {
              // EOF
              return ss::make_ready_future<>();
          }
          auto size = process_size(_read_buf, std::move(buf));
          // Allow for extra copies and bookkeeping
          auto mem_estimate = size * 2 + 8000;
          if (mem_estimate >= _server._max_request_size) {
              // TODO: Create error response using the specific API?
              throw std::runtime_error(fmt::format(
                "Request size is too large (size: {}; estimate: {}; allowed: "
                "{}",
                size,
                mem_estimate,
                _server._max_request_size));
          }
          auto fut = get_units(_server._memory_available, mem_estimate);
          if (_server._memory_available.waiters()) {
              _server._probe.waiting_for_available_memory();
          }
          return fut.then([this, size](ss::semaphore_units<> units) {
              return read_header(_read_buf).then(
                [this, size, units = std::move(units)](
                  request_header header) mutable {
                    // update the throughput tracker for this client using the
                    // size of the current request and return any computed delay
                    // to apply for quota throttling.
                    //
                    // note that when throttling is first applied the request is
                    // allowed to pass through and subsequent requests and
                    // delayed. this is a similar strategy used by kafka: the
                    // response is important because it allows clients to
                    // distinguish throttling delays from real delays. delays
                    // applied to subsequent messages allow backpressure to take
                    // affect.
                    auto delay
                      = _server._quota_mgr.local().record_tp_and_throttle(
                        header.client_id, size);

                    // apply the throttling delay, if any.
                    auto throttle_delay = delay.first_violation
                                            ? ss::make_ready_future<>()
                                            : ss::sleep(delay.duration);
                    return throttle_delay.then([this,
                                                size,
                                                header = std::move(header),
                                                units = std::move(units),
                                                delay = std::move(
                                                  delay)]() mutable {
                        auto remaining = size - sizeof(raw_request_header)
                                         - header.client_id_buffer.size();
                        return read_iobuf_exactly(_read_buf, remaining)
                          .then([this,
                                 header = std::move(header),
                                 units = std::move(units),
                                 delay = std::move(delay)](iobuf buf) mutable {
                              auto ctx = request_context(
                                _server._metadata_cache,
                                _server._cntrl_dispatcher.local(),
                                std::move(header),
                                std::move(buf),
                                delay.duration,
                                _server._group_router.local(),
                                _server._shard_table.local(),
                                _server._partition_manager);
                              _server._probe.serving_request();
                              do_process(std::move(ctx), std::move(units));
                          });
                    });
                });
          });
      });
}

void kafka_server::connection::do_process(
  request_context&& ctx, ss::semaphore_units<>&& units) {
    auto correlation = ctx.header().correlation_id;
    auto f = ::kafka::process_request(std::move(ctx), _server._smp_group);
    auto ready = std::move(_ready_to_respond);
    _ready_to_respond = f.then_wrapped(
      [this, units = std::move(units), ready = std::move(ready), correlation](
        ss::future<response_ptr>&& f) mutable {
          try {
              auto r = f.get0();
              return ready
                .then([this, r = std::move(r), correlation]() mutable {
                    return write_response(std::move(r), correlation);
                })
                .then([this] { _server._probe.request_served(); });
          } catch (...) {
              _server._probe.request_processing_error();
              klog.debug(
                "Failed to process request: {}", std::current_exception());
              return std::move(ready);
          }
      });
}

size_t kafka_server::connection::process_size(
  const ss::input_stream<char>& src, ss::temporary_buffer<char>&& buf) {
    if (src.eof()) {
        return 0;
    }
    auto* raw = ss::unaligned_cast<const size_type*>(buf.get());
    size_type size = be_to_cpu(*raw);
    if (size < 0) {
        throw std::runtime_error(
          fmt::format("Invalid request size of {}", size));
    }
    return size_t(size);
}

ss::future<request_header>
kafka_server::connection::read_header(ss::input_stream<char>& src) {
    constexpr int16_t no_client_id = -1;
    return src.read_exactly(sizeof(raw_request_header))
      .then([&src](ss::temporary_buffer<char> buf) {
          if (src.eof()) {
              throw std::runtime_error(
                fmt::format("Unexpected EOF for request header"));
          }
          auto client_id_size = be_to_cpu(
            reinterpret_cast<const raw_request_header*>(buf.get())
              ->client_id_size);
          auto make_header = [buf = std::move(buf)]() -> request_header {
              auto* raw_header = reinterpret_cast<const raw_request_header*>(
                buf.get());
              return request_header{
                api_key(ss::net::ntoh(raw_header->api_key)),
                api_version(ss::net::ntoh(raw_header->api_version)),
                ss::net::ntoh(raw_header->correlation_id)};
          };
          if (client_id_size == 0) {
              auto header = make_header();
              header.client_id = std::string_view();
              return ss::make_ready_future<request_header>(std::move(header));
          }
          if (client_id_size == no_client_id) {
              return ss::make_ready_future<request_header>(make_header());
          }
          return src.read_exactly(client_id_size)
            .then([&src, make_header = std::move(make_header)](
                    ss::temporary_buffer<char> buf) {
                if (src.eof()) {
                    throw std::runtime_error(
                      fmt::format("Unexpected EOF for client ID"));
                }
                auto header = make_header();
                header.client_id_buffer = std::move(buf);
                header.client_id = std::string_view(
                  header.client_id_buffer.get(),
                  header.client_id_buffer.size());
                validate_utf8(*header.client_id);
                return header;
            });
      });
}

} // namespace kafka
