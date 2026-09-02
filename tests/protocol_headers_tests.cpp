#include "nstu/protocol_headers.h"

#include <cassert>
#include <cstddef>
#include <cstring>

int main() {
    nstu::wire::ProtocolPrefix prefix{
        {'N', 'S', 'T', 'U'},
        nstu::wire::kProtocolVersion,
        static_cast<std::uint8_t>(nstu::wire::GatewayRole::client),
        static_cast<std::uint8_t>(nstu::wire::GatewayMessage::hello),
        7,
        static_cast<std::uint16_t>(sizeof(nstu::wire::GatewayHandshake)),
    };
    assert(std::memcmp(prefix.magic, nstu::wire::kProtocolMagic,
                       sizeof(prefix.magic)) == 0);
    assert(sizeof(prefix) == nstu::wire::kProtocolPrefixBytes);
    assert(sizeof(nstu::wire::GatewayHandshake) ==
           nstu::wire::kGatewayHandshakeBytes);
    assert(sizeof(nstu::wire::ExamSyncPayload) ==
           nstu::wire::kExamSyncHeaderBytes);
    assert(sizeof(nstu::wire::ExamSyncAckPayload) ==
           nstu::wire::kExamSyncAckBytes);
    return 0;
}
