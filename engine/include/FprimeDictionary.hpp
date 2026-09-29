#pragma once

#include <dartwic/EngineProtocol.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace FPrimeBridge {

struct EncodedCommand {
    std::uint64_t opcode = 0;
    std::vector<std::uint8_t> arguments;
};

struct DecodedChannel {
    std::string name;
    DARTWIC::Value value;
    bool numeric = false;
    bool decoded = false;
    std::string error;
};

struct DecodedEvent {
    std::string name;
    std::string format;
    std::string severity;
    DARTWIC::Value::Object arguments;
    bool decoded = false;
    std::string error;
};

struct CommandArgument {
    std::string name;
    std::string type;
    std::string description;
    std::vector<DARTWIC::Value> choices;
};

struct CommandDescriptor {
    std::string name;
    std::string description;
    std::vector<CommandArgument> arguments;
};

/**
 * Runtime decoder for an F Prime topology JSON dictionary.
 *
 * Keeping dictionary interpretation here avoids generated, deployment-specific
 * switch statements in the bridge. Primitive, string, alias, enum, array, and
 * struct values are supported.
 */
class Dictionary final {
  public:
    Dictionary();
    ~Dictionary();
    Dictionary(Dictionary&&) noexcept;
    Dictionary& operator=(Dictionary&&) noexcept;
    Dictionary(const Dictionary&) = delete;
    Dictionary& operator=(const Dictionary&) = delete;

    bool load(const std::string& path, std::string& error);
    EncodedCommand encodeCommand(const std::string& name, const DARTWIC::Value::Object& arguments,
                                 std::size_t maximumBytes) const;
    void clear();
    bool loaded() const noexcept;

    DecodedChannel decodeChannel(std::uint64_t id, const std::uint8_t* data, std::size_t size) const;
    DecodedChannel decodeChannelPrefix(std::uint64_t id, const std::uint8_t* data,
                                       std::size_t available, std::size_t& consumed) const;
    DecodedEvent decodeEvent(std::uint64_t id, const std::uint8_t* data, std::size_t size) const;
    std::size_t channelBytes(std::uint64_t id) const;
    std::string commandWithSuffix(const std::string& suffix) const;
    std::vector<CommandDescriptor> commands() const;
    bool hasCommand(const std::string& name) const;

    static std::string fallbackChannelName(std::uint64_t id);
    static std::string fallbackEventName(std::uint64_t id);
    static std::string hex(const std::uint8_t* data, std::size_t size);
    static std::string peerChannelName(const std::string& fprimeName);

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace FPrimeBridge
