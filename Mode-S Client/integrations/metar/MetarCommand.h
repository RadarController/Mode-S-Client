#pragma once

#include <string>

namespace metar {

// Returns true when the message was recognised as a !metar command, even if lookup fails.
// On success, outReply contains the text to send back to chat.
// On lookup errors, outLogError receives a diagnostic string suitable for logging.
bool TryGetMetarReply(const std::string& messageText,
    std::string& outReply,
    std::string* outLogError = nullptr);

} // namespace metar
