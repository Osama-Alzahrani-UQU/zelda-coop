// The mod's settings, kept in bin\coop\coop_settings.ini (key=value lines).
#pragma once

#include <string>

#include "session.h"

namespace coop {

struct Config {
    std::string name;            // the player's name (asked once, changed in Online Settings)
    std::string last_host;       // the last server joined by address (a.b.c.d:port)
    HostSettings host;
    GuestSettings guest;
};

Config& config();
void config_save();

}  // namespace coop
