#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace noemancer {

// Performs a real kernel UDP exchange over the loopback interface. The
// returned document is bounded and contains no native socket handles.
[[nodiscard]] std::string verify_udp_loopback_transport_json(std::size_t payload_bytes = 256);
[[nodiscard]] std::string run_network_server_json(std::uint16_t port, std::uint32_t session_budget = 1,
                                                  std::uint32_t timeout_milliseconds = 5000);
[[nodiscard]] std::string run_network_client_json(std::string_view host, std::uint16_t port,
                                                  std::string_view peer_id = "client.local",
                                                  std::size_t state_payload_bytes = 256,
                                                  std::uint32_t timeout_milliseconds = 5000);

} // namespace noemancer
