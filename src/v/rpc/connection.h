#pragma once

#include "rpc/batched_output_stream.h"
#include "rpc/server_probe.h"
#include "seastarx.h"

#include <seastar/core/iostream.hh>
#include <seastar/net/api.hh>
#include <seastar/net/socket_defs.hh>

#include <boost/intrusive/list.hpp>

namespace rpc {

class connection : public boost::intrusive::list_base_hook<> {
public:
    connection(
      boost::intrusive::list<connection>& hook,
      ss::connected_socket f,
      ss::socket_address a,
      server_probe& p);
    ~connection();
    connection(const connection&) = delete;
    ss::input_stream<char>& input() { return _in; }
    ss::future<> write(ss::scattered_message<char> msg);
    ss::future<> shutdown();

    const ss::socket_address addr;

private:
    std::reference_wrapper<boost::intrusive::list<connection>> _hook;
    ss::connected_socket _fd;
    ss::input_stream<char> _in;
    batched_output_stream _out;
    server_probe& _probe;
};
} // namespace rpc
