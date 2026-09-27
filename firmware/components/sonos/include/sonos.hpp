#pragma once
#include <functional>
#include <map>
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
    bool muted = false;
};
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
    void play_favorite(const Room& room, const Favorite& favorite);
    void play_queue_track(const Room& room, int number);
    void transport(const Room& room, const std::string& action);
    void volume(const Room& room, int value, bool group = false);
    void mute(const Room& room, bool value);
    void join(const Room& room, const Room& destination);
    void ungroup(const Room& room);
    void apply_area(const std::string& seed, const std::vector<std::string>& room_ids);
    // Coordinator lookups reuse the last topology until this is called, a
    // grouping change is made, or a room is missing from it.
    void invalidate_topology() { topology_.clear(); }
private:
    Transport transport_;
    Fields services_;
    std::vector<Room> topology_;
    Fields call(const std::string& ip, const std::string& service,
                const std::string& action, const Fields& fields = {});
    Room coordinator(const Room& room);
};
}
