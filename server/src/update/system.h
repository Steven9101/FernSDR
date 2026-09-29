// The updater's environment on a real machine: releases fetched with curl
// (or wget), the new version's configuration check run as the receiver's
// user, and the receiver's service driven through systemctl.
#pragma once
#include <sys/types.h>

#include <string>

#include "updater.h"

namespace fernsdr {

// Where releases are published: the latest release's assets on GitHub.
constexpr const char* kReleaseBaseUrl = "https://github.com/Steven9101/FernSDR/releases/latest/download/";

// The updater's environment for this machine. The service unit may set
// FERNSDR_UPDATE_URL (an https:// address ending in '/') and
// FERNSDR_UPDATE_TRIAL_SECONDS (90 to 900); they are for trying an update
// against a release server of one's own. False, with the reason, when a
// setting is wrong.
bool system_update_environment(UpdateEnvironment& environment, std::string& error);

// Where releases are fetched from: kReleaseBaseUrl, or FERNSDR_UPDATE_URL
// when the environment sets it, which has to be an https:// address ending
// in '/'. False, with the reason, when it is not.
bool release_base_url(std::string& url, std::string& error);

// Fetches `url`, at most `limit` bytes, with curl, or with wget where there is
// no curl; curl is held to HTTPS for every redirect.
bool fetch_release_file(const std::string& url, size_t limit, std::string& body, std::string& error);

// Runs `program --check config --root web` as uid:gid, root dropped for good
// first (supplementary groups, group, user), with a minimal environment and
// a minute to finish. True when it exits 0; otherwise `output` says why.
bool check_configuration_as(const std::string& program, const std::string& config, const std::string& web,
                            uid_t uid, gid_t gid, std::string& output);

}  // namespace fernsdr
