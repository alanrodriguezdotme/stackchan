/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 * SPDX-FileCopyrightText: 2026 Alan Rodriguez
 *
 * SPDX-License-Identifier: MIT
 */
#include "app_dance.h"
#include <hal/hal.h>
#include <mooncake.h>
#include <mooncake_log.h>
#include <stackchan/stackchan.h>
#include <apps/common/common.h>
#include <assets/assets.h>
#include <smooth_ui_toolkit.hpp>
#include <smooth_lvgl.hpp>
#include <lvgl.h>
#include <fmt/format.h>
#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>

using namespace mooncake;
using namespace stackchan;
using namespace smooth_ui_toolkit::lvgl_cpp;

namespace {

constexpr uint32_t kThemePrimary   = 0xB77BFF;
constexpr uint32_t kThemeSecondary = 0x422268;

// Compact bottom overlay so "it didn't dance" is diagnosable: three band meters
// (bass / mid / treble), a BPM readout and a dot that flashes on every detected
// beat. Lets you tell "didn't hear it" from "heard it and moved badly".
class DanceDebugView {
public:
    // onCycleSensitivity is invoked when the user taps the panel; it returns the
    // name of the newly-selected preset so the label can reflect it live.
    explicit DanceDebugView(std::function<const char*()> onCycleSensitivity)
        : _on_cycle_sensitivity(std::move(onCycleSensitivity))
    {
        _panel = std::make_unique<Container>(lv_screen_active());
        _panel->setBgColor(lv_color_hex(kThemeSecondary));
        _panel->setBgOpa(LV_OPA_50);
        _panel->setScrollbarMode(LV_SCROLLBAR_MODE_OFF);
        _panel->setBorderWidth(0);
        _panel->setRadius(0);
        _panel->setSize(320, 34);
        _panel->setPadding(0, 0, 0, 0);
        _panel->align(LV_ALIGN_BOTTOM_MID, 0, 0);
        _panel->addFlag(LV_OBJ_FLAG_CLICKABLE);
        _panel->onClick().connect([this]() {
            if (_on_cycle_sensitivity) {
                _sens_label->setText(_on_cycle_sensitivity());
            }
        });

        const char* labels[kNumBands] = {"B", "M", "T"};
        const uint32_t colors[kNumBands] = {0xFF5C5C, 0x5CFF8F, 0x5CC8FF};
        for (int i = 0; i < kNumBands; ++i) {
            _band_label[i] = std::make_unique<Label>(_panel->get());
            _band_label[i]->setText(labels[i]);
            _band_label[i]->setTextColor(lv_color_hex(0xFFFFFF));
            _band_label[i]->setTextFont(&lv_font_montserrat_14);
            _band_label[i]->align(LV_ALIGN_LEFT_MID, 6, -9 + i * 9);

            _band[i] = std::make_unique<Bar>(_panel->get());
            _band[i]->setSize(150, 6);
            _band[i]->setRange(0, 100);
            _band[i]->setValue(0);
            _band[i]->setBgColor(lv_color_hex(0x202020));
            _band[i]->setBgColor(lv_color_hex(colors[i]), LV_PART_INDICATOR);
            _band[i]->align(LV_ALIGN_LEFT_MID, 24, -9 + i * 9);
        }

        _bpm_label = std::make_unique<Label>(_panel->get());
        _bpm_label->setText("-- BPM");
        _bpm_label->setTextColor(lv_color_hex(0xFFFFFF));
        _bpm_label->setTextFont(&lv_font_montserrat_16);
        _bpm_label->align(LV_ALIGN_RIGHT_MID, -34, 0);

        // Tappable sensitivity readout: tap the panel to cycle LOW/MED/HIGH.
        _sens_label = std::make_unique<Label>(_panel->get());
        _sens_label->setText("MED");
        _sens_label->setTextColor(lv_color_hex(kThemePrimary));
        _sens_label->setTextFont(&lv_font_montserrat_14);
        _sens_label->align(LV_ALIGN_CENTER, 30, -8);

        _beat_dot = std::make_unique<Container>(_panel->get());
        _beat_dot->setSize(16, 16);
        _beat_dot->setRadius(8);
        _beat_dot->setBorderWidth(0);
        _beat_dot->setBgColor(lv_color_hex(0x303030));
        _beat_dot->align(LV_ALIGN_RIGHT_MID, -10, 0);
    }

    void update(const ReactiveDanceModifier::DebugState& s, uint32_t now)
    {
        _band[0]->setValue(scale(s.bass.load(), 320.0f));
        _band[1]->setValue(scale(s.mid.load(), 380.0f));
        _band[2]->setValue(scale(s.treble.load(), 460.0f));

        const bool active = s.active.load();
        const float bpm   = s.bpm.load();
        if (!active) {
            _bpm_label->setText("LISTEN");
        } else if (bpm > 1.0f) {
            _bpm_label->setText(fmt::format("{:.0f} BPM", bpm));
        } else {
            _bpm_label->setText("-- BPM");
        }

        // Beat dot: white flash on each detected beat while dancing; a dim blue
        // "armed" glow between beats; near-black while idle/listening.
        const bool lit = active && (now - s.lastBeatMs.load()) < 120;
        uint32_t dot   = 0x202020;
        if (lit) {
            dot = 0xFFFFFF;
        } else if (active) {
            dot = 0x2E6BFF;
        }
        _beat_dot->setBgColor(lv_color_hex(dot));
    }

private:
    static constexpr int kNumBands = 3;

    static int32_t scale(float energy, float gain)
    {
        int v = static_cast<int>(energy * gain);
        return std::min(100, std::max(0, v));
    }

    std::unique_ptr<Container> _panel;
    std::unique_ptr<Label> _band_label[kNumBands];
    std::unique_ptr<Bar> _band[kNumBands];
    std::unique_ptr<Label> _bpm_label;
    std::unique_ptr<Label> _sens_label;
    std::unique_ptr<Container> _beat_dot;
    std::function<const char*()> _on_cycle_sensitivity;
};

}  // namespace

struct AppDance::Impl {
    std::unique_ptr<DanceDebugView> debug_view;
    ReactiveDanceModifier* modifier = nullptr;
};

AppDance::AppDance() : _impl(std::make_unique<Impl>())
{
    setAppInfo().name = "DANCE";
    static auto icon  = assets::get_image("icon_dance.bin");
    setAppInfo().icon = (void*)&icon;
    static uint32_t theme_color = kThemePrimary;
    setAppInfo().userData       = (void*)&theme_color;
}

AppDance::~AppDance() = default;

void AppDance::onCreate()
{
    mclog::tagInfo(getAppInfo().name, "on create");
}

void AppDance::onOpen()
{
    mclog::tagInfo(getAppInfo().name, "on open");

    // Start streaming mic capture before anything reads frames.
    GetHAL().startAudioReactiveCapture();

    LvglLockGuard lock;

    // Avatar.
    auto avatar = std::make_unique<avatar::DefaultAvatar>();
    avatar->init(lv_screen_active());
    GetStackChan().attachAvatar(std::move(avatar));

    // We drive the head continuously; auto angle sync would fight us.
    GetStackChan().motion().setAutoAngleSyncEnabled(false);

    // Attach the reactive dance brain and keep a handle for the debug view.
    _modifier_id     = GetStackChan().addModifier(std::make_unique<ReactiveDanceModifier>());
    _impl->modifier  = static_cast<ReactiveDanceModifier*>(GetStackChan().getModifier(_modifier_id));
    _impl->debug_view = std::make_unique<DanceDebugView>([this]() -> const char* {
        if (!_impl->modifier) {
            return "MED";
        }
        const int preset = _impl->modifier->cycleSensitivityPreset();
        return ReactiveDanceModifier::sensitivityPresetName(preset);
    });

    // Common widgets.
    view::create_home_indicator([&]() { close(); }, kThemePrimary, kThemeSecondary);
    view::create_status_bar(kThemePrimary, kThemeSecondary);
}

void AppDance::onRunning()
{
    LvglLockGuard lvgl_lock;

    GetStackChan().update();

    if (_impl->debug_view && _impl->modifier) {
        _impl->debug_view->update(_impl->modifier->debug(), GetHAL().millis());
    }

    view::update_home_indicator();
    view::update_status_bar();
}

void AppDance::onClose()
{
    mclog::tagInfo(getAppInfo().name, "on close");

    GetHAL().stopAudioReactiveCapture();

    {
        LvglLockGuard lock;

        if (_modifier_id >= 0) {
            GetStackChan().removeModifier(_modifier_id);
            _modifier_id    = -1;
            _impl->modifier = nullptr;
        }
        _impl->debug_view.reset();

        GetStackChan().motion().setAutoAngleSyncEnabled(true);
        GetStackChan().motion().goHome(500);
        GetStackChan().resetAvatar();

        view::destroy_home_indicator();
        view::destroy_status_bar();
    }

    GetHAL().requestWarmReboot(5);
}
