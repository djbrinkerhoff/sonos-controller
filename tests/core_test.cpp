#include "sonos.hpp"

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <iostream>
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
    expect(mutations.size() == 4, "queued favorite should add, select queue, seek, then play");
    expect(mutations[0].action == "AddURIToQueue" && mutations[0].ip == "192.168.1.2", "favorite was not queued on live coordinator");
    expect(argument_names(mutations[0]) == std::vector<std::string>({"InstanceID", "EnqueuedURI", "EnqueuedURIMetaData", "DesiredFirstTrackNumberEnqueued", "EnqueueAsNext"}),
           "AddURIToQueue SOAP argument order changed");
    expect(mutations[1].action == "SetAVTransportURI" && mutations[2].action == "Seek" && mutations[3].action == "Play",
           "queue playback command sequence changed");
    expect(mutations[1].body.find("x-rincon-queue:RINCON_COORD#0") != std::string::npos, "queue URI did not use coordinator ID");
    expect(argument_names(mutations[3]) == std::vector<std::string>({"InstanceID", "Speed"}), "Play arguments were not ordered");
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

int main() {
    try {
        parsing_and_validation();
        providers_fail_closed();
        topology_and_coordinator();
        commands_and_paging();
        update_changes_and_argument_order();
        std::cout << "core tests passed (" << checks << " assertions)\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "core test failed after " << checks << " assertions: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
