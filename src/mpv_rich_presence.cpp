// This file is part of mpv-rich-presence.
// Copyright (c) 2026 Alden Wu.
//
// mpv-rich-presence is free software: you can redistribute it and/or modify it
// under the terms of the GNU Affero General Public License as published by the
// Free Software Foundation, either version 3 of the License, or (at your
// option) any later version.
//
// mpv-rich-presence is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
// FITNESS FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License
// for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with mpv-rich-presence. If not, see <https://www.gnu.org/licenses/>.

#include <boost/dll.hpp>
#include <boost/url.hpp>
#include <cmrc/cmrc.hpp>
#include <mpv/client.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <regex>
#include <string>
#include <thread>

#include "mpv_rich_presence/mpv_utils.hpp"
#include "mpv_rich_presence/rich_presence_state.hpp"

namespace chrono = std::chrono;
namespace fs = std::filesystem;
namespace dll = boost::dll;
using namespace std::literals;
using namespace mpvrp;

static constexpr chrono::seconds SLEEP_DURATION = 1s;
static const std::string DISCORD_SDK_DIR = "discord_social_sdk";

CMRC_DECLARE(mpvrp);

static void handle_client_message(rich_presence_state& state, int64_t& application_id, mpv_event_client_message* msg)
{
    if (msg->num_args == 0)
        return;

    auto is_enabled_prev = state.is_enabled;

    if (msg->args[0] == "toggle"s)
    {
        state.is_enabled = !state.is_enabled.has_value() || !*state.is_enabled;
    }
    else if (msg->args[0] == "on"s)
    {
        state.is_enabled = true;
    }
    else if (msg->args[0] == "off"s)
    {
        state.is_enabled = false;
    }
    else if (msg->args[0] == "application_id"s)
    {
        if (msg->num_args < 2)
        {
            mpv_print(state.mpv, "Not enough args for application_id");
        }
        else
        {
            try
            {
                application_id = std::stoll(msg->args[1]);
                mpv_print(state.mpv, std::format("Received application_id: {}", application_id));
            }
            catch (const std::exception& e)
            {
                mpv_print(state.mpv, e.what() + ": "s + msg->args[1]);
            }
        }
    }

    if (is_enabled_prev.has_value() && state.is_enabled.has_value() && state.is_enabled != is_enabled_prev)
        mpv_show(state.mpv, std::format("Rich presence {}", *state.is_enabled ? "enabled" : "disabled"));
}

static std::string format_artists(std::string artists);

static void handle_file_loaded(rich_presence_state& state)
{
    state.media_has_audio = false;
    state.media_has_video = false;

    int64_t media_track_count = 0;
    mpv_get_property(state.mpv, "track-list/count", MPV_FORMAT_INT64, &media_track_count);
    for (int64_t i = 0; i < media_track_count; i++)
    {
        const char* media_track_type = "";
        mpv_get_property(state.mpv, std::format("track-list/{}/type", i).c_str(), MPV_FORMAT_STRING, &media_track_type);
        if (media_track_type == "audio"s)
            state.media_has_audio = true;

        if (media_track_type == "video"s)
        {
            int is_image = 0;
            mpv_get_property(state.mpv, std::format("track-list/{}/image", i).c_str(), MPV_FORMAT_FLAG, &is_image);
            if (is_image == 0)
                state.media_has_video = true;
        }
    }

    const char* media_filename = "";
    const char* media_artist = "";
    const char* media_title = "";
    mpv_get_property(state.mpv, "filename", MPV_FORMAT_OSD_STRING, &media_filename);
    mpv_get_property(state.mpv, "metadata/by-key/Artist", MPV_FORMAT_OSD_STRING, &media_artist);
    mpv_get_property(state.mpv, "media-title", MPV_FORMAT_OSD_STRING, &media_title);
    state.media_filename = media_filename == nullptr ? "" : media_filename;
    state.media_artist = media_artist == nullptr ? "" : media_artist;
    state.media_artist = format_artists(state.media_artist);
    state.media_title = media_title == nullptr ? "" : media_title;
}

static std::string format_artists(std::string artists)
{
    std::string result;
    size_t start = 0;

    while (start < artists.size())
    {
        size_t end = artists.find(';', start);

        std::string artist = artists.substr(
            start,
            end == std::string::npos ? std::string::npos : end - start
        );

        // Trim leading/trailing whitespace
        const auto first = artist.find_first_not_of(" \t");
        const auto last = artist.find_last_not_of(" \t");

        if (first != std::string::npos)
        {
            artist = artist.substr(first, last - first + 1);

            if (!result.empty())
                result += " • ";

            result += artist;
        }

        if (end == std::string::npos)
            break;

        start = end + 1;
    }

    return result;
}

static std::string get_mpv_string(mpv_handle* mpv, const char* name)
{
    std::string result;
    if (char* value = mpv_get_property_string(mpv, name))
    {
        result = value;
        mpv_free(value);
    }
    return result;
}

// "Album (Year)", or empty if there is no album tag or it just repeats the title (singles)
static std::string format_album(mpv_handle* mpv, const std::string& title)
{
    auto album = get_mpv_string(mpv, "metadata/by-key/Album");
    auto same_char = [](unsigned char a, unsigned char b) { return std::tolower(a) == std::tolower(b); };
    if (album.empty() || (album.size() == title.size() && std::equal(album.begin(), album.end(), title.begin(), same_char)))
        return {};

    auto date = get_mpv_string(mpv, "metadata/by-key/Date");
    if (date.size() >= 4 && std::all_of(date.begin(), date.begin() + 4, [](unsigned char c) { return std::isdigit(c); }))
        album += std::format(" ({})", date.substr(0, 4));

    return album;
}

// "OPUS • 193 kbps • 5.1 MiB • x1.1"
// Skip any metric mpv can't report
static std::string format_media_info(mpv_handle* mpv, bool has_video)
{
    std::string result;
    auto append = [&result](const std::string& part) {
        if (part.empty())
            return;
        if (!result.empty())
            result += " • ";
        result += part;
    };

    auto codec = get_mpv_string(mpv, has_video ? "current-tracks/video/codec" : "current-tracks/audio/codec");
    std::transform(codec.begin(), codec.end(), codec.begin(), [](unsigned char c) { return std::toupper(c); });
    append(codec);

    int64_t height = 0;
    if (has_video && mpv_get_property(mpv, "height", MPV_FORMAT_INT64, &height) >= 0 && height > 0)
        append(std::format("{}p", height));

    int64_t size_bytes = 0;
    if (mpv_get_property(mpv, "file-size", MPV_FORMAT_INT64, &size_bytes) >= 0 && size_bytes > 0)
    {
        // average bitrate (audio only, it's rarely interesting for video)
        double duration_s = 0.0;
        if (!has_video && mpv_get_property(mpv, "duration", MPV_FORMAT_DOUBLE, &duration_s) >= 0 && duration_s > 0.0)
            append(std::format("{:.0f} kbps", size_bytes * 8.0 / duration_s / 1'000.0));

        constexpr double MIB = 1024.0 * 1024.0;
        const double size_mib = size_bytes / MIB;
        append(size_mib >= 1024.0 ? std::format("{:.1f} GiB", size_mib / 1024.0) : std::format("{:.1f} MiB", size_mib));
    }

    double speed = 1.0;
    if (mpv_get_property(mpv, "speed", MPV_FORMAT_DOUBLE, &speed) >= 0 && std::abs(speed - 1.0) > 0.005)
        append(std::format("x{:g}", speed));

    return result;
}

// Discord drops the entire activity if any text field is over 128 bytes. "•" and "—" are 3 bytes each in UTF-8
// The limits are counted in bytes, not characters
static constexpr size_t DISCORD_MAX_FIELD_BYTES = 128;
static constexpr std::string_view FIELD_SEPARATOR = " • ";

// Cuts a string down to max_bytes without splitting a UTF-8 character
static std::string truncate_utf8(std::string s, size_t max_bytes = DISCORD_MAX_FIELD_BYTES)
{
    if (s.size() <= max_bytes)
        return s;

    constexpr std::string_view ELLIPSIS = "…";
    size_t cut = max_bytes - ELLIPSIS.size();
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) // skip UTF-8 continuation bytes
        --cut;
    s.resize(cut);
    while (!s.empty() && s.back() == ' ')
        s.pop_back();
    return s + std::string { ELLIPSIS };
}

// "Title — Album" if it fits, otherwise just the truncated title
static std::string fit_details(const std::string& title, const std::string& album)
{
    if (!album.empty())
    {
        auto full = title + " — " + album;
        if (full.size() <= DISCORD_MAX_FIELD_BYTES)
            return full;
    }
    return truncate_utf8(title);
}

// "Artist • Artist • ... • info". Drops artists from the end until it fits
// If first artist doesn't fit alongside the info, the info is dropped and the first artist is truncated
static std::string fit_state(std::string primary, const std::string& info)
{
    auto join = [](const std::string& a, const std::string& b) {
        return a.empty() || b.empty() ? a + b : a + std::string { FIELD_SEPARATOR } + b;
    };

    while (join(primary, info).size() > DISCORD_MAX_FIELD_BYTES)
    {
        auto pos = primary.rfind(FIELD_SEPARATOR);
        if (pos == std::string::npos)
            return truncate_utf8(primary.empty() ? info : primary);
        primary.resize(pos);
    }

    return join(primary, info);
}

void on_discord_log(Discord_String msg, Discord_LoggingSeverity severity, void* payload)
{
    auto& state = *(rich_presence_state*)payload;

    auto severity_str = ""s;
    switch (severity)
    {
    case Discord_LoggingSeverity::Verbose:
        severity_str = "Verbose";
        break;
    case Discord_LoggingSeverity::Info:
        severity_str = "Info";
        break;
    case Discord_LoggingSeverity::Warning:
        severity_str = "Warning";
        break;
    case Discord_LoggingSeverity::Error:
        severity_str = "Error";
        break;
    case Discord_LoggingSeverity::None:
        severity_str = "None";
        break;
    default:
        severity_str = "unknown";
        break;
    }

    auto msg_view = std::string_view { msg.data, msg.size };
    mpv_print(state.mpv, std::format("[{}] {}", severity_str, msg_view));
}

void on_discord_update_rich_presence(Discord_ClientResult* result, void* payload)
{
    auto& state = *(rich_presence_state*)payload;

    if (state.discord_api->Discord_ClientResult_Successful(result))
        return;

    int32_t code = state.discord_api->Discord_ClientResult_ErrorCode(result);
    mpv_print(state.mpv, std::format("Rich presence error: {}", code));
}

auto mpv_open_cplugin_impl(mpv_handle* ctx) -> int
{
    auto state = rich_presence_state {};
    state.mpv = ctx;

    // Initialization

    mpv_print(state.mpv, "Pinging rich_presence_conf for config data...");
    auto ping_args = std::array<const char*, 4> { "script-message-to", "rich_presence_conf", "ping", nullptr };
    mpv_command(state.mpv, ping_args.data());

    // these are to deal with synchronization issues, not really state
    std::atomic<bool> is_ready = false;
    int64_t application_id = 0;

    mpv_print(state.mpv, "Initializing Discord Social SDK...");
    auto discord_init = std::jthread { [&state, &is_ready]() {
        auto embedded_fs = cmrc::mpvrp::get_filesystem();
        auto filename = (*embedded_fs.iterate_directory(DISCORD_SDK_DIR).begin()).filename();
        auto embedded_sdk = embedded_fs.open(std::format("{}/{}", DISCORD_SDK_DIR, filename));
        auto temp_sdk_path = fs::temp_directory_path() / filename;
        {
            auto temp_sdk = std::ofstream { temp_sdk_path, std::ios_base::binary };
            temp_sdk.write(embedded_sdk.begin(), embedded_sdk.size());
        }
        auto sdk = dll::shared_library { fs::absolute(temp_sdk_path).string() };
        state.discord_api = std::make_shared<discord_api_importer>(sdk);

        state.discord = std::make_unique<discord_client>(state.discord_api);
        state.discord_api->Discord_Client_AddLogCallback(&state.discord->get(), on_discord_log, nullptr, &state, Discord_LoggingSeverity::Warning);

        is_ready = true;
    } };

    // Main event loop

    while (true)
    {
        // Event handling (+ sleep)

        auto* event = mpv_wait_event(state.mpv, chrono::duration_cast<chrono::milliseconds>(SLEEP_DURATION).count() * 1e-3);

        if (event->event_id == MPV_EVENT_SHUTDOWN)
            break;

        if (event->event_id == MPV_EVENT_CLIENT_MESSAGE)
            handle_client_message(state, application_id, (mpv_event_client_message*)event->data);
        else if (event->event_id == MPV_EVENT_FILE_LOADED)
            handle_file_loaded(state);

        // Rich Presence updating

        if (!is_ready)
            continue;

        state.discord_api->Discord_RunCallbacks();

        if (state.discord_api->Discord_Client_GetApplicationId(&state.discord->get()) != application_id)
            state.discord_api->Discord_Client_SetApplicationId(&state.discord->get(), application_id);

        if (application_id == 0)
            continue;

        if (!state.is_enabled.has_value()
            || !*state.is_enabled
            || (!state.media_has_audio && !state.media_has_video))
        {
            state.discord_api->Discord_Client_ClearRichPresence(&state.discord->get());
            continue;
        }

        double media_time_pos_s = 0.0;
        double media_time_left_s = 0.0;
        int is_media_paused = 0;
        mpv_get_property(state.mpv, "time-pos/full", MPV_FORMAT_DOUBLE, &media_time_pos_s);
        mpv_get_property(state.mpv, "time-remaining/full", MPV_FORMAT_DOUBLE, &media_time_left_s);
        mpv_get_property(state.mpv, "pause", MPV_FORMAT_FLAG, &is_media_paused);

        if (is_media_paused == 1)
        {
            state.discord_api->Discord_Client_ClearRichPresence(&state.discord->get());
            continue;
        }

        auto activity = discord_activity { state.discord_api };
        auto activity_type = state.media_has_video ? Discord_ActivityTypes::Watching : Discord_ActivityTypes::Listening;

        std::regex basename(R"((.*)\.\w\w\w\w?$)");
        auto activity_name = std::regex_replace(state.media_filename, basename, "$1");

        auto display_type = Discord_StatusDisplayTypes::Name;

        std::string state_string = "";
        std::string details_string = state.media_title;
        bool is_series = false;

        if (activity_type == Discord_ActivityTypes::Watching)
        {
            std::regex pattern(R"((.*) S(\d+)E(\d+)\.\w\w\w\w?$)");
            is_series = std::regex_match(state.media_filename, pattern);

            if (is_series)
            {
                activity_name = std::regex_replace(state.media_filename, pattern, "$1");

                auto s = std::regex_replace(state.media_filename, pattern, "$2");
                s.erase(0, s.find_first_not_of('0'));
                auto e = std::regex_replace(state.media_filename, pattern, "$3");
                e.erase(0, e.find_first_not_of('0'));

                state_string = std::format("Season {} Episode {}", s, e);
            }
        } else if (!state.media_artist.empty()) {
            activity_name = "music";

            // boost::urls::url url("https://www.youtube.com/results");
            // url.params().append({"search_query", state.media_artist + " " + state.media_title});
            // auto activity_details_url_string = url.buffer();
            // auto activity_details_url = Discord_String { activity_details_url_string.data(), activity_details_url_string.size() };
            // state.discord_api->Discord_Activity_SetDetailsUrl(&activity.get(), &activity_details_url);

            display_type = Discord_StatusDisplayTypes::State;
            state_string = state.media_artist;

            details_string = fit_details(state.media_title, format_album(state.mpv, state.media_title));
        }

        // Make sure every field stays within Discord's limit, otherwise the whole presence vanishes
        state_string = fit_state(state_string, format_media_info(state.mpv, state.media_has_video));
        details_string = truncate_utf8(details_string);
        activity_name = truncate_utf8(activity_name);

        auto activity_details = Discord_String { details_string.data(), details_string.size() };
        auto activity_state = Discord_String { state_string.data(), state_string.size() };
        state.discord_api->Discord_Activity_SetType(&activity.get(), activity_type);
        state.discord_api->Discord_Activity_SetStatusDisplayType(&activity.get(), &display_type);
        state.discord_api->Discord_Activity_SetName(&activity.get(), { activity_name.data(), activity_name.size() });
        state.discord_api->Discord_Activity_SetDetails(&activity.get(), is_series || !state.media_artist.empty() ? &activity_details : nullptr);
        state.discord_api->Discord_Activity_SetState(&activity.get(), is_series || !state_string.empty() ? &activity_state : nullptr);

        auto timestamps = discord_activity_timestamps { state.discord_api };
        uint64_t now_ms = chrono::duration_cast<chrono::milliseconds>(chrono::system_clock::now().time_since_epoch()).count();
        state.discord_api->Discord_ActivityTimestamps_SetStart(&timestamps.get(), now_ms - (media_time_pos_s * 1'000));
        state.discord_api->Discord_ActivityTimestamps_SetEnd(&timestamps.get(), now_ms + (media_time_left_s * 1'000));
        state.discord_api->Discord_Activity_SetTimestamps(&activity.get(), &timestamps.get());

        state.discord_api->Discord_Client_UpdateRichPresence(&state.discord->get(), &activity.get(), on_discord_update_rich_presence, nullptr, &state);
    }

    // Shut down

    mpv_print(state.mpv, "Shutting down...");
    if (is_ready)
        state.discord_api->Discord_Client_ClearRichPresence(&state.discord->get());

    return EXIT_SUCCESS;
}

extern "C" MPV_EXPORT auto mpv_open_cplugin(mpv_handle* ctx) -> int
{
    try
    {
        return mpv_open_cplugin_impl(ctx);
    }
    catch (const std::exception& e)
    {
        mpv_print(ctx, std::format("Encountered exception: {}", e.what()));
    }
    return EXIT_FAILURE;
}