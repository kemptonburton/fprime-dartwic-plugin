#pragma once
#include <tempest/Transport.h>
namespace TEMPEST::detail {
// Internal connection ownership. Public transport implementations only use
// receive(), setConnected(), start(), send(), and stop().
struct TransportAccess {
  static void bind(Transport &transport, std::function<void(Message)> message,
                   std::function<void(bool, std::string)> state) {
    transport.endpoint_.store(std::make_shared<Transport::Endpoint>(
        Transport::Endpoint{std::move(message), std::move(state)}));
  }
  static void clear(Transport &transport) { transport.endpoint_.store({}); }
  static auto receiver(Transport &transport) {
    auto endpoint =
        std::weak_ptr<Transport::Endpoint>(transport.endpoint_.load());
    return [endpoint](Message message) {
      if (auto target = endpoint.lock(); target && target->message)
        target->message(std::move(message));
    };
  }
  static auto stateReporter(Transport &transport) {
    auto endpoint =
        std::weak_ptr<Transport::Endpoint>(transport.endpoint_.load());
    return [endpoint](bool connected, std::string reason) {
      if (auto target = endpoint.lock(); target && target->state)
        target->state(connected, std::move(reason));
    };
  }
  static void deliver(Transport &transport, Message message) {
    transport.receive(std::move(message));
  }
  static void state(Transport &transport, bool connected,
                    std::string reason = {}) {
    transport.setConnected(connected, std::move(reason));
  }
};
} // namespace TEMPEST::detail
