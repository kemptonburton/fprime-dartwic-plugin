#pragma once

#include <sdk_api.h>
#include <functional>
#include <string>
#include <utility>

namespace DARTWIC::API {
    // Poll from the plugin's owning loop. Callbacks run on that loop, never on a
    // transport thread, so plugin lifetime and locking remain under its control.
    // This helper adds no virtual methods or ABI requirements to SDK_API.
    class NotificationMuteHandle {
    public:
        NotificationMuteHandle(std::string local_id,
                               std::function<void()> on_mute = {},
                               std::function<void()> on_unmute = {})
            : local_id_(std::move(local_id)), on_mute_(std::move(on_mute)),
              on_unmute_(std::move(on_unmute)) {}

        bool refresh(SDK_API& api) {
            const bool muted = api.isNotificationMuted(local_id_);
            if ((!initialized_ && muted) || (initialized_ && muted != muted_)) {
                if (muted && on_mute_) on_mute_();
                if (!muted && on_unmute_) on_unmute_();
            }
            initialized_ = true;
            muted_ = muted;
            return muted_;
        }

    private:
        std::string local_id_;
        std::function<void()> on_mute_;
        std::function<void()> on_unmute_;
        bool initialized_{false};
        bool muted_{false};
    };
}
