#include "sonos.hpp"

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
int checks = 0;

void expect(bool condition, const std::string& message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}

template <typename F> void expect_throw(F&& function, const std::string& message) {
    bool threw = false;
    try { function(); } catch (const std::exception&) { threw = true; }
    expect(threw, message);
}

std::string xml_escape(const std::string& value) {
    std::string result;
    for (char c : value) {
        switch (c) {
            case '&': result += "&amp;"; break;
            case '<': result += "&lt;"; break;
            case '>': result += "&gt;"; break;
            case '"': result += "&quot;"; break;
            case '\'': result += "&apos;"; break;
            default: result += c;
        }
    }
    return result;
}

std::string soap(const std::string& fields) {
    return "<s:Envelope xmlns:s=\"urn:soap\"><s:Body><u:Response xmlns:u=\"urn:test\">" +
           fields + "</u:Response></s:Body></s:Envelope>";
}

std::string metadata(const std::string& service_type = "775",
                     const std::string& media_class = "object.item.audioItem.musicTrack") {
    return "<DIDL-Lite><item><title>Track</title><desc>SA_RINCON" + service_type +
           "_ACCOUNT</desc><class>" + media_class + "</class></item></DIDL-Lite>";
}

std::string favorite_item(const std::string& uri,
                          const std::string& service_type = "775",
                          const std::string& title = "Track") {
    const auto md = metadata(service_type);
    return "<item id=\"fav-1\"><title>" + xml_escape(title) + "</title><res>" +
           xml_escape(uri) + "</res><resMD>" + xml_escape(md) +
           "</resMD><albumArtURI>/art</albumArtURI></item>";
}

std::string topology(const std::string& coordinator_ip = "192.168.1.2",
                     const std::string& coordinator_id = "RINCON_COORD",
                     const std::string& extra = "") {
    const auto xml = "<ZoneGroups><ZoneGroup Coordinator=\"" + coordinator_id +
        "\"><ZoneGroupMember UUID=\"RINCON_SAT\" ZoneName=\"Satellite\" Location=\"http://192.168.1.3:1400/xml/device_description.xml\"/>" +
        "<ZoneGroupMember UUID=\"RINCON_COORD\" ZoneName=\"Coordinator\" Location=\"http://" +
        coordinator_ip + ":1400/xml/device_description.xml\"/>" + extra +
        "</ZoneGroup></ZoneGroups>";
    return soap("<ZoneGroupState>" + xml_escape(xml) + "</ZoneGroupState>");
}

std::vector<std::string> argument_names(const sonos::Request& request) {
    std::vector<std::string> names;
    size_t pos = 0;
    while ((pos = request.body.find('<', pos)) != std::string::npos) {
        if (pos + 1 >= request.body.size() || request.body[pos + 1] == '/' || request.body[pos + 1] == '?') {
            ++pos;
            continue;
        }
        auto end = request.body.find_first_of(" />", pos + 1);
        if (end == std::string::npos) break;
        auto name = request.body.substr(pos + 1, end - pos - 1);
        if (name == "InstanceID" || name == "Channel" || name == "ObjectID" || name == "BrowseFlag" ||
            name == "Filter" || name == "StartingIndex" || name == "RequestedCount" || name == "SortCriteria" ||
            name == "DesiredVolume" || name == "DesiredMute" ||
            name == "Speed" || name == "EnqueuedURI" || name == "EnqueuedURIMetaData" ||
            name == "DesiredFirstTrackNumberEnqueued" || name == "EnqueueAsNext" || name == "CurrentURI" ||
            name == "CurrentURIMetaData" || name == "Unit" || name == "Target") names.push_back(name);
        pos = end + 1;
    }
    return names;
}

void parsing_and_validation() {
    expect(sonos::valid_ipv4("192.168.1.2"), "valid IPv4 rejected");
    expect(!sonos::valid_ipv4("192.168.1.256"), "out-of-range IPv4 accepted");
    expect(!sonos::valid_ipv4("1.2.3"), "short IPv4 accepted");
    expect(sonos::ipv4_from_url("http://10.0.0.4:1400/path") == "10.0.0.4", "speaker URL IP not parsed");
    expect(sonos::ipv4_from_url("https://10.0.0.4/path").empty(), "non-HTTP URL accepted");
    expect(sonos::escape("<&>\"'") == "&lt;&amp;&gt;&quot;&apos;", "SOAP XML metacharacters not escaped");

    const auto parsed = sonos::parse_response(soap("<Value>A&amp;B</Value>"));
    expect(parsed.at("Value") == "A&B", "SOAP response entities not decoded");
    expect_throw([] { sonos::parse_response("<broken>"); }, "malformed SOAP response accepted");
    expect_throw([] { sonos::parse_response("<!DOCTYPE a [<!ENTITY x 'x'>]><a/>"); }, "DOCTYPE accepted");
    expect_throw([] { sonos::parse_response(std::string(512 * 1024 + 1, 'x')); }, "oversized XML accepted");
    expect_throw([] { sonos::parse_response("<s:Envelope><s:Body><s:Fault><detail><errorCode>701</errorCode><errorDescription>bad</errorDescription></detail></s:Fault></s:Body></s:Envelope>"); },
                 "SOAP fault treated as success");
}

void providers_fail_closed() {
    const sonos::Fields providers{{"3", "Apple Music"}, {"4", "Sonos Radio"}, {"8", "Another Provider"}};
    auto apple = sonos::parse_favorites("<DIDL-Lite>" + favorite_item("x-sonos-http:track?sid=3") + "</DIDL-Lite>", providers);
    expect(apple.size() == 1 && apple[0].provider == "Apple Music", "known Apple Music favorite not recognized");

    const auto radio_md = metadata("1031", "object.item.audioItem.audioBroadcast");
    const auto radio = "<item><title>Radio</title><res>x-sonosapi-stream:station?sid=4</res><resMD>" +
        xml_escape(radio_md) + "</resMD></item>";
    auto radio_favorites = sonos::parse_favorites("<DIDL-Lite>" + radio + "</DIDL-Lite>", providers);
    expect(radio_favorites.size() == 1 && radio_favorites[0].provider == "Sonos Radio" && radio_favorites[0].radio,
           "Sonos Radio broadcast not recognized as radio");

    auto unknown = sonos::parse_favorites("<DIDL-Lite>" + favorite_item("x-sonos-http:track?sid=8", "2055") +
                                          "</DIDL-Lite>", providers);
    expect(unknown.empty(), "unsupported music provider was exposed");
    auto conflict = sonos::parse_favorites("<DIDL-Lite>" + favorite_item("x-sonos-http:track?sid=4", "775") +
                                           "</DIDL-Lite>", providers);
    expect(conflict.empty(), "conflicting provider identities were accepted");
    auto malformed = sonos::parse_favorites("<DIDL-Lite><item><res>uri</res><resMD>&lt;broken&gt;</resMD></item></DIDL-Lite>", providers);
    expect(malformed.empty(), "malformed favorite metadata escaped fail-closed filtering");
}

void topology_and_coordinator() {
    const std::string xml = "<Event><ZoneGroups><ZoneGroup Coordinator=\"RINCON_MAIN\">"
        "<ZoneGroupMember UUID=\"RINCON_SAT\" ZoneName=\"Kitchen\" Location=\"http://192.168.1.3:1400/x\"/>"
        "<ZoneGroupMember UUID=\"RINCON_MAIN\" ZoneName=\"Living Room\" Location=\"http://192.168.1.2:1400/x\"/>"
        "<ZoneGroupMember UUID=\"RINCON_BONDED\" ZoneName=\"Sub\" Location=\"http://192.168.1.4:1400/x\" Invisible=\"1\"/>"
        "<ZoneGroupMember UUID=\"RINCON_BAD\" ZoneName=\"No IP\" Location=\"http://speaker.local:1400/x\"/>"
        "</ZoneGroup></ZoneGroups></Event>";
    auto rooms = sonos::parse_rooms(xml);
    expect(rooms.size() == 2, "invisible bonded or invalid host appeared as room");
    expect(rooms[0].name == "Kitchen" && rooms[0].coordinator == "RINCON_MAIN", "satellite topology was not retained/sorted");
    expect(rooms[1].id == "RINCON_MAIN", "coordinator topology missing");
    expect_throw([] { sonos::parse_rooms("<Event/>"); }, "missing ZoneGroups accepted");

    std::vector<sonos::Request> requests;
    sonos::Client client([&](const sonos::Request& request) {
        requests.push_back(request);
        if (request.action == "GetZoneGroupState") return topology("192.168.1.9", "RINCON_COORD");
        return soap("");
    });
    const sonos::Room satellite{"RINCON_SAT", "Satellite", "192.168.1.3", "OLD_COORD"};
    client.transport(satellite, "Play");
    expect(requests.size() == 2, "transport did not refresh the coordinator before acting");
    expect(requests[1].ip == "192.168.1.9" && requests[1].action == "Play", "transport used the stale coordinator");
    expect(argument_names(requests[1]) == std::vector<std::string>({"InstanceID", "Speed"}), "Play SOAP argument order/values changed");
    expect_throw([&] { client.transport(satellite, "Reboot"); }, "unsupported transport action accepted");

    requests.clear();
    client.transport(satellite, "Pause");
    client.transport(satellite, "Play");
    expect(requests.size() == 2 && requests[0].action == "Pause" && requests[1].action == "Play",
           "coordinator lookups did not reuse the cached topology");
    client.invalidate_topology();
    client.transport(satellite, "Pause");
    expect(requests.size() == 4 && requests[2].action == "GetZoneGroupState", "invalidated topology was not refetched");
    requests.clear();
    const sonos::Room unknown{"RINCON_NEW", "New", "192.168.1.9", "RINCON_COORD"};
    expect_throw([&] { client.transport(unknown, "Play"); }, "room missing from fresh topology was accepted");
    expect(requests.size() == 1 && requests[0].action == "GetZoneGroupState", "room missing from cache did not force a refetch");
}

void commands_and_paging() {
    std::vector<sonos::Request> requests;
    sonos::Client client([&](const sonos::Request& request) {
        requests.push_back(request);
        if (request.service.find(":MusicServices:") != std::string::npos) {
            return soap("<AvailableServiceDescriptorList>" +
                        xml_escape("<Services><Service Id=\"3\" Name=\"Apple Music\"/></Services>") +
                        "</AvailableServiceDescriptorList>");
        }
        if (request.service.find(":ContentDirectory:") != std::string::npos) {
            const bool second = request.body.find("<StartingIndex>1</StartingIndex>") != std::string::npos;
            const auto list = "<DIDL-Lite>" + favorite_item("x-sonos-http:track" + std::to_string(second ? 2 : 1) + "?sid=3", "775", second ? "Second" : "First") + "</DIDL-Lite>";
            return soap("<NumberReturned>1</NumberReturned><TotalMatches>2</TotalMatches><UpdateID>7</UpdateID><Result>" +
                        xml_escape(list) + "</Result>");
        }
        if (request.action == "GetZoneGroupState") return topology();
        if (request.action == "AddURIToQueue") return soap("<FirstTrackNumberEnqueued>2</FirstTrackNumberEnqueued>");
        return soap("");
    });
    auto favorites = client.favorites("192.168.1.2");
    expect(favorites.size() == 2 && favorites[1].title == "Second", "favorite pagination failed");
    std::vector<std::string> starts;
    for (const auto& request : requests) if (request.action == "Browse") {
        starts.push_back(request.body.find("<StartingIndex>1</StartingIndex>") != std::string::npos ? "1" : "0");
        expect(argument_names(request) == std::vector<std::string>({"ObjectID", "BrowseFlag", "Filter", "StartingIndex", "RequestedCount", "SortCriteria"}),
               "Browse SOAP argument order changed");
    }
    expect(starts == std::vector<std::string>({"0", "1"}), "favorites request did not advance both pages");

    const sonos::Room satellite{"RINCON_SAT", "Satellite", "192.168.1.3", "OLD_COORD"};
    client.play_favorite(satellite, favorites.front());
    std::vector<sonos::Request> mutations;
    for (const auto& request : requests) if (request.action != "ListAvailableServices" && request.action != "Browse" && request.action != "GetZoneGroupState") mutations.push_back(request);
    expect(mutations.size() == 5, "queued favorite should clear, add, select queue, seek, then play");
    expect(mutations[0].action == "RemoveAllTracksFromQueue" && mutations[0].ip == "192.168.1.2", "queue was not cleared on live coordinator");
    expect(mutations[1].action == "AddURIToQueue" && mutations[1].ip == "192.168.1.2", "favorite was not queued on live coordinator");
    expect(argument_names(mutations[1]) == std::vector<std::string>({"InstanceID", "EnqueuedURI", "EnqueuedURIMetaData", "DesiredFirstTrackNumberEnqueued", "EnqueueAsNext"}),
           "AddURIToQueue SOAP argument order changed");
    expect(mutations[2].action == "SetAVTransportURI" && mutations[3].action == "Seek" && mutations[4].action == "Play",
           "queue playback command sequence changed");
    expect(mutations[2].body.find("x-rincon-queue:RINCON_COORD#0") != std::string::npos, "queue URI did not use coordinator ID");
    expect(argument_names(mutations[4]) == std::vector<std::string>({"InstanceID", "Speed"}), "Play arguments were not ordered");

    requests.clear();
    const auto radio_md = metadata("775", "object.item.audioItem.audioBroadcast");
    auto radio = sonos::parse_favorites("<DIDL-Lite><item><title>Station</title><res>x-sonosapi-stream:s1?sid=3</res><resMD>" +
                                        xml_escape(radio_md) + "</resMD></item></DIDL-Lite>", {{"3", "Apple Music"}});
    expect(radio.size() == 1 && radio[0].radio, "radio favorite fixture not recognized");
    client.play_favorite(satellite, radio[0]);
    mutations.clear();
    for (const auto& request : requests) if (request.action != "GetZoneGroupState") mutations.push_back(request);
    expect(mutations.size() == 2 && mutations[0].action == "SetAVTransportURI" && mutations[1].action == "Play",
           "radio favorite should set the stream and play without touching the queue");
    expect(mutations[0].body.find("x-sonosapi-stream:s1?sid=3") != std::string::npos, "radio stream URI not sent");
}

void update_changes_and_argument_order() {
    int browse_page = 0;
    sonos::Client client([&](const sonos::Request& request) {
        if (request.service.find(":MusicServices:") != std::string::npos)
            return soap("<AvailableServiceDescriptorList>&lt;Services/&gt;</AvailableServiceDescriptorList>");
        if (request.service.find(":ContentDirectory:") != std::string::npos) {
            ++browse_page;
            return soap(std::string("<NumberReturned>1</NumberReturned><TotalMatches>2</TotalMatches><UpdateID>") +
                        (browse_page == 1 ? "1" : "2") + "</UpdateID><Result>&lt;DIDL-Lite/&gt;</Result>");
        }
        return soap("");
    });
    expect_throw([&] { client.favorites("192.168.1.2"); }, "changed favorites list returned partial data");

    std::vector<sonos::Request> volume_requests;
    sonos::Client controls([&](const sonos::Request& request) {
        volume_requests.push_back(request);
        return request.action == "GetZoneGroupState" ? topology() : soap("");
    });
    const sonos::Room room{"RINCON_COORD", "Coordinator", "192.168.1.2", "RINCON_COORD"};
    controls.volume(room, 35);
    controls.mute(room, true);
    expect(argument_names(volume_requests[0]) == std::vector<std::string>({"InstanceID", "Channel", "DesiredVolume"}), "SetVolume arguments are out of order");
    expect(argument_names(volume_requests[1]) == std::vector<std::string>({"InstanceID", "Channel", "DesiredMute"}), "SetMute arguments are out of order");
    expect(volume_requests[0].body.find("<DesiredVolume>35</DesiredVolume>") != std::string::npos, "volume value missing");
    expect(volume_requests[1].body.find("<DesiredMute>1</DesiredMute>") != std::string::npos, "mute value missing");
    expect_throw([&] { controls.volume(room, 101); }, "out-of-range volume accepted");
}
}

// A small in-memory household: topology reflects join/ungroup mutations, so
// multi-step grouping logic is exercised against changing coordinators.
struct Household {
    struct Member { std::string name, ip, coordinator; };
    std::map<std::string, Member> rooms;
    std::vector<sonos::Request> mutations;

    std::string id_at(const std::string& ip) const {
        for (const auto& [id, room] : rooms) if (room.ip == ip) return id;
        throw std::runtime_error("request sent to unknown speaker " + ip);
    }
    std::string state() const {
        std::map<std::string, std::string> groups;
        for (const auto& [id, room] : rooms)
            groups[room.coordinator] += "<ZoneGroupMember UUID=\"" + id + "\" ZoneName=\"" + room.name +
                "\" Location=\"http://" + room.ip + ":1400/x\"/>";
        std::string xml = "<ZoneGroups>";
        for (const auto& [coordinator, members] : groups)
            xml += "<ZoneGroup Coordinator=\"" + coordinator + "\">" + members + "</ZoneGroup>";
        return soap("<ZoneGroupState>" + xml_escape(xml + "</ZoneGroups>") + "</ZoneGroupState>");
    }
    std::string operator()(const sonos::Request& request) {
        if (request.action == "GetZoneGroupState") return state();
        mutations.push_back(request);
        const auto id = id_at(request.ip);
        if (request.action == "BecomeCoordinatorOfStandaloneGroup") {
            std::string heir;
            for (auto& [other, room] : rooms) if (other != id && room.coordinator == id) {
                if (heir.empty()) heir = other;
                room.coordinator = heir;
            }
            rooms[id].coordinator = id;
        } else if (request.action == "SetAVTransportURI") {
            const std::string prefix = "x-rincon:";
            auto at = request.body.find(prefix);
            if (at != std::string::npos) rooms[id].coordinator = request.body.substr(at + prefix.size(), request.body.find('<', at) - at - prefix.size());
        }
        return soap("");
    }
    std::vector<std::string> log() const {
        std::vector<std::string> result;
        for (const auto& m : mutations) result.push_back(m.action + "@" + id_at(m.ip));
        return result;
    }
    sonos::Room room(const std::string& id) const {
        const auto& r = rooms.at(id);
        return {id, r.name, r.ip, r.coordinator};
    }
};

void grouping_and_areas() {
    auto household = std::make_shared<Household>();
    household->rooms = {{"A", {"Kitchen", "10.0.0.1", "A"}}, {"B", {"Den", "10.0.0.2", "A"}},
                        {"C", {"Patio", "10.0.0.3", "C"}}, {"D", {"Office", "10.0.0.4", "D"}}};
    sonos::Client client([household](const sonos::Request& r) { return (*household)(r); });

    client.join(household->room("C"), household->room("B"));
    expect(household->rooms["C"].coordinator == "A", "join did not target the destination's coordinator");
    expect(household->mutations.back().body.find("x-rincon:A<") != std::string::npos, "join URI did not name the coordinator");
    household->mutations.clear();
    client.join(household->room("A"), household->room("B"));
    expect(household->mutations.empty(), "joining a coordinator to its own group sent a command");

    client.ungroup(household->room("C"));
    expect(household->log() == std::vector<std::string>({"BecomeCoordinatorOfStandaloneGroup@C"}), "ungroup was not sent to the room itself");
    household->mutations.clear();

    // Area {B, D}: A is an outsider in B's group, so B leaves it and D joins B.
    client.apply_area("10.0.0.1", {"B", "D"});
    expect(household->log() == std::vector<std::string>({"BecomeCoordinatorOfStandaloneGroup@B", "SetAVTransportURI@D"}),
           "area with an outsider should split off the first room, then join the rest");
    expect(household->rooms["B"].coordinator == "B" && household->rooms["D"].coordinator == "B" && household->rooms["A"].coordinator == "A",
           "area membership was not restored");
    household->mutations.clear();

    // Area {D, B, C}: B already leads {B, D}; only C needs to join, and to B.
    client.apply_area("10.0.0.1", {"D", "B", "C", "B"});
    expect(household->log() == std::vector<std::string>({"SetAVTransportURI@C"}),
           "area should keep the existing coordinator and skip rooms already grouped");
    expect(household->rooms["C"].coordinator == "B", "area room joined the wrong coordinator");
    household->mutations.clear();

    expect_throw([&] { client.apply_area("10.0.0.1", {"B", "MISSING"}); }, "area with an unavailable room was applied");
    expect(household->mutations.empty(), "unavailable area room still changed grouping");
    expect_throw([&] { client.apply_area("10.0.0.1", {}); }, "empty area was applied");
}

void group_volume_and_state() {
    std::vector<sonos::Request> requests;
    const std::string track = "<DIDL-Lite><item><dc:title>Song</dc:title><dc:creator>Artist</dc:creator>"
        "<upnp:album>Album</upnp:album><upnp:albumArtURI>/art</upnp:albumArtURI></item></DIDL-Lite>";
    sonos::Client client([&](const sonos::Request& request) {
        requests.push_back(request);
        if (request.action == "GetZoneGroupState") return topology("192.168.1.9", "RINCON_COORD");
        if (request.action == "GetPositionInfo") return soap("<TrackMetaData>" + xml_escape(track) + "</TrackMetaData>");
        if (request.action == "GetTransportInfo") return soap("<CurrentTransportState>PLAYING</CurrentTransportState>");
        if (request.action == "GetCurrentTransportActions") return soap("<Actions>Play, Stop,Pause,Next</Actions>");
        if (request.action == "GetVolume") return soap("<CurrentVolume>12</CurrentVolume>");
        if (request.action == "GetMute") return soap("<CurrentMute>1</CurrentMute>");
        if (request.action == "GetGroupVolume") return soap("<CurrentVolume>40</CurrentVolume>");
        return soap("");
    });
    const sonos::Room satellite{"RINCON_SAT", "Satellite", "192.168.1.3", "RINCON_COORD"};

    client.volume(satellite, 30, true);
    expect(requests.size() == 3 && requests[1].action == "SnapshotGroupVolume" && requests[2].action == "SetGroupVolume",
           "group volume should snapshot, then set");
    expect(requests[1].ip == "192.168.1.9" && requests[2].ip == "192.168.1.9", "group volume did not target the coordinator");
    expect(argument_names(requests[2]) == std::vector<std::string>({"InstanceID", "DesiredVolume"}), "SetGroupVolume arguments changed");
    expect(requests[2].body.find("<DesiredVolume>30</DesiredVolume>") != std::string::npos, "group volume value missing");

    requests.clear();
    const auto state = client.state(satellite);
    expect(state.title == "Song" && state.artist == "Artist" && state.album == "Album" && state.art == "/art", "track metadata not parsed");
    expect(state.playback == "PLAYING" && state.volume == 12 && state.muted && state.group_volume == 40, "state values not parsed");
    for (const auto& request : requests) {
        const bool room_level = request.action == "GetVolume" || request.action == "GetMute";
        if (request.action != "GetZoneGroupState")
            expect(request.ip == (room_level ? "192.168.1.3" : "192.168.1.9"), request.action + " sent to the wrong speaker");
    }
}

int main() {
    try {
        parsing_and_validation();
        providers_fail_closed();
        topology_and_coordinator();
        commands_and_paging();
        update_changes_and_argument_order();
        grouping_and_areas();
        group_volume_and_state();
        std::cout << "core tests passed (" << checks << " assertions)\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "core test failed after " << checks << " assertions: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
