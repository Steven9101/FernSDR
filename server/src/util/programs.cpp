#include "programs.h"

#include <unistd.h>

#include <cstdlib>

namespace fernsdr {

std::string find_program(const char* name) {
    for (const char* directory : {"/usr/bin/", "/usr/local/bin/", "/bin/"}) {
        const std::string candidate = std::string(directory) + name;
        if (::access(candidate.c_str(), X_OK) == 0) return candidate;
    }
    return "";
}

std::vector<std::string> network_helper_environment() {
    std::vector<std::string> out = {"PATH=/usr/local/bin:/usr/bin:/bin", "LANG=C.UTF-8"};
    for (const char* name : {"https_proxy", "HTTPS_PROXY", "no_proxy", "NO_PROXY", "SSL_CERT_FILE", "SSL_CERT_DIR",
                             "CURL_CA_BUNDLE"}) {
        if (const char* value = std::getenv(name)) out.push_back(std::string(name) + "=" + value);
    }
    return out;
}

}  // namespace fernsdr
