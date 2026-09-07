#pragma once
#include "modules/Interfaces.h"
namespace corevideo::modules {
std::unique_ptr<IOutputSender> createIsolatedOutputSender(
    std::vector<std::unique_ptr<IOutputSender>> senders,
    std::vector<std::string> supportedDestinations = {});
}
