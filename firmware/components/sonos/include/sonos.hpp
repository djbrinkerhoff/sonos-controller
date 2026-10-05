#pragma once
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace sonos {
using Fields = std::map<std::string, std::string>;
struct Request { std::string ip, path, service, action, body; };
// Return the complete response XML or throw. Never retry a mutating request.
using Transport = std::function<std::string(const Request&)>;
struct Room { std::string id, name, ip, coordinator; };
struct Favorite {
    std::string id, title, uri, metadata, art, provider;
    bool radio = false;
};
struct QueueItem { int number; std::string title, artist, album, art; };
struct State {
    std::string title, artist, album, art, playback, actions;
    int volume = 0;
    int group_volume = 0;
    int track = 0;
    int position = -1, duration = -1;  // seconds; -1 when unknown (e.g. radio)
    bool muted = false, group_muted = false;
};
// What a group is playing, for room overviews: two calls instead of state()'s six.
struct Summary { std::string title, artist, playback; };
// One room's own volume, as opposed to its group's.
struct Level { int volume = 0; bool muted = false; };
// Thrown by multi-request reads when the callback given to set_interrupt()
// asks them to stop between calls, so queued commands can run first. Any
// partial result is discarded; the read can simply be started again.
struct Preempted : std::runtime_error { Preempted() : std::runtime_error("read preempted by a queued command") {} };
int parse_clock(const std::string& hms);  // "H:MM:SS" -> seconds, -1 if not a time
// What the Sonos app calls a source that has no track metadata: "TV" for a
// soundbar's HDMI/optical input, "Line-In" for analog in; "" for anything else.
std::string source_title(const std::string& track_uri);
std::string escape(const std::string& value);
Fields parse_response(const std::string& xml);
std::vector<Room> parse_rooms(const std::string& xml);
Fields parse_services(const std::string& xml);
std::vector<Favorite> parse_favorites(const std::string& xml, const Fields& services);
std::vector<QueueItem> parse_queue(const std::string& didl, int first_number);
std::string ipv4_from_url(const std::string& url);
bool valid_ipv4(const std::string& ip);

class Client {
public:
    explicit Client(Transport transport) : transport_(std::move(transport)) {}
    std::vector<Room> rooms(const std::string& seed);
    std::vector<Favorite> favorites(const std::string& seed);
    // start is a 0-based queue index; items are numbered from start + 1.
    std::vector<QueueItem> queue(const Room& room, int start, int count, int* total = nullptr);
    State state(const Room& room);
    Summary summary(const Room& coordinator);
    void play_favorite(const Room& room, const Favorite& favorite);
    void play_queue_track(const Room& room, int number);
    void transport(const Room& room, const std::string& action);
    void volume(const Room& room, int value, bool group = false);
    void mute(const Room& room, bool value, bool group = false);
    void join(const Room& room, const Room& destination);
    void ungroup(const Room& room);
    // Takes the room out of its group and leaves the music with the others. A
    // coordinator first hands the group to another member (Sonos's
    // DelegateGroupCoordinationTo); plain ungrouping would keep the music on
    // it and stop everyone else. A room alone is left as it is.
    void leave(const Room& room);
    Level level(const Room& room);
    void apply_area(const std::string& seed, const std::vector<std::string>& room_ids);
    // Coordinator lookups reuse the last topology until this is called, a
    // grouping change is made, or a room is missing from it.
    void invalidate_topology() { topology_.clear(); }
    Room coordinator(const Room& room);
    // Checked between the requests of multi-call reads (state, favorites);
    // return true to abandon the read early with Preempted.
    void set_interrupt(std::function<bool()> interrupt) { interrupt_ = std::move(interrupt); }
private:
    Transport transport_;
    std::function<bool()> interrupt_;
    Fields services_;
    std::vector<Room> topology_;
    Fields call(const std::string& ip, const std::string& service,
                const std::string& action, const Fields& fields = {});
};
}
