/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 * SPDX-FileCopyrightText: 2026 Alan Rodriguez
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <mooncake.h>
#include <memory>

/**
 * @brief Standalone beat-reactive dance mode.
 *
 * Listens to music through the mics and dances on its own - no phone, no server,
 * no BLE. The heavy lifting lives in stackchan::ReactiveDanceModifier and the
 * pure DSP core under stackchan::audioreactive.
 */
class AppDance : public mooncake::AppAbility {
public:
    AppDance();
    ~AppDance() override;

    void onCreate() override;
    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;

    int _modifier_id = -1;
};
