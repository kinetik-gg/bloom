#pragma once

// The network-share helpers live in the kit beside kit::chooseFile(), which adds the mounted shares
// to every file dialog's sidebar. Panel code keeps reaching them by their bloom::ui names.
#include <bloom/ui/kit/network_shares.hpp>

namespace bloom::ui {
using kit::configureFileDialogSidebar;
using kit::defaultGvfsRoot;
using kit::localPathForUrl;
using kit::mountedNetworkShares;
using kit::smbShareLabel;
} // namespace bloom::ui
