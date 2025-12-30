#pragma once
#include <string>

class ObsWsClient {
public:
    bool connect(const std::string& host, int port, const std::string& password) {
        (void)host; (void)port; (void)password;
        return false; // stub for now
    }
    void set_text(const std::string& inputName, const std::string& text) {
        (void)inputName; (void)text;
    }
};
