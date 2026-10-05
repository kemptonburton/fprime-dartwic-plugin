#include "FprimeDictionary.hpp"

#include <array>
#include <iostream>
#include <stdexcept>

static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

int main(int argc, char** argv) {
    try {
        require(argc == 3, "pass the command type and custom boolean fixture dictionaries");
        FPrimeBridge::Dictionary dictionary;
        std::string error;
        require(dictionary.load(argv[1], error), error.c_str());
        using DARTWIC::Value;
        for (const bool enabled : {true, false}) {
            const auto command = dictionary.encodeCommand("Test.DEBUG_DRAW",
                {{"axisLengthM", 5}, {"enabled", enabled}}, 5);
            require(command.opcode == 0x1000A007, "command opcode changed");
            require(command.arguments == std::vector<std::uint8_t>{
                static_cast<std::uint8_t>(enabled ? 0xFF : 0x00), 0x40, 0xA0, 0x00, 0x00},
                "boolean/F32 command wire representation or argument order is wrong");
        }
        const auto nested = dictionary.encodeCommand("Test.OPTIONS",
            {{"options", Value::Object{{"enabled", true}, {"flags", Value::Array{false, true}}}}}, 3);
        require(nested.arguments == std::vector<std::uint8_t>{0xFF, 0x00, 0xFF},
            "nested alias/struct/array boolean encoding is wrong");
        bool rejected = false;
        try {
            dictionary.encodeCommand("Test.DEBUG_DRAW", {{"enabled", true}, {"axisLengthM", 5}}, 4);
        } catch (const std::invalid_argument&) { rejected = true; }
        require(rejected, "command buffer limit was not enforced");

        for (const auto raw : std::array<std::uint8_t, 4>{0xFF, 0x00, 0x01, 0xFE}) {
            const bool valid = raw == 0xFF || raw == 0x00;
            for (const auto channelId : {1U, 2U}) {
                const auto channel = dictionary.decodeChannel(channelId, &raw, 1);
                require(channel.decoded == valid, "boolean telemetry validity is wrong");
                if (valid) require(std::get<bool>(channel.value.storage()) == (raw == 0xFF),
                    "boolean telemetry value is wrong");
                std::size_t consumed = 99;
                const auto prefix = dictionary.decodeChannelPrefix(channelId, &raw, 1, consumed);
                require(prefix.decoded == valid && consumed == (valid ? 1 : 0),
                    "boolean telemetry prefix validity or consumption is wrong");
            }
            const auto event = dictionary.decodeEvent(1, &raw, 1);
            require(event.decoded == valid, "boolean event validity is wrong");
            if (valid) require(std::get<bool>(event.arguments.at("enabled").storage()) == (raw == 0xFF),
                "boolean event value is wrong");
        }
        const auto empty = dictionary.decodeChannel(1, nullptr, 0);
        require(!empty.decoded, "empty boolean telemetry was accepted");
        require(dictionary.load(argv[2], error), error.c_str());
        for (const bool enabled : {true, false}) {
            const auto raw = static_cast<std::uint8_t>(enabled ? 0xAA : 0x55);
            const auto command = dictionary.encodeCommand("Test.ENABLE", {{"enabled", enabled}}, 1);
            require(command.arguments == std::vector<std::uint8_t>{raw},
                "dictionary boolean constant was ignored during encoding");
            const auto channel = dictionary.decodeChannel(1, &raw, 1);
            require(channel.decoded && std::get<bool>(channel.value.storage()) == enabled,
                "dictionary boolean constant was ignored during decoding");
        }
        const std::uint8_t standardTrue = 0xFF;
        require(!dictionary.decodeChannel(1, &standardTrue, 1).decoded,
            "default boolean byte was accepted with custom constants");
        require(dictionary.load(argv[1], error), error.c_str());
        require(dictionary.decodeChannel(1, &standardTrue, 1).decoded,
            "reloading an older dictionary did not restore boolean defaults");
        std::cout << "F Prime boolean command and telemetry serialization passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
