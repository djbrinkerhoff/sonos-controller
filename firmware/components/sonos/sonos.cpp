#include "sonos.hpp"
#include "tinyxml2.h"
#include <algorithm>
#include <cctype>
#include <climits>
#include <set>
#include <stdexcept>

namespace sonos {
namespace {
using tinyxml2::XMLElement;
std::string local(const char* value) {
    std::string s = value ? value : "";
    auto colon = s.find(':');
    return colon == std::string::npos ? s : s.substr(colon + 1);
}
const XMLElement* child(const XMLElement* parent, const std::string& name) {
    if (parent) for (auto e = parent->FirstChildElement(); e; e = e->NextSiblingElement())
        if (local(e->Name()) == name) return e;
    return nullptr;
}
const XMLElement* descendant(const XMLElement* parent, const std::string& name) {
    if (!parent) return nullptr;
    if (local(parent->Name()) == name) return parent;
    for (auto e = parent->FirstChildElement(); e; e = e->NextSiblingElement())
        if (auto result = descendant(e, name)) return result;
    return nullptr;
}
std::string text(const XMLElement* e) { return e && e->GetText() ? e->GetText() : ""; }
std::string attr(const XMLElement* e, const char* key) {
    return e && e->Attribute(key) ? e->Attribute(key) : "";
}
void parse(tinyxml2::XMLDocument& doc, const std::string& xml) {
    if (xml.size() > 512 * 1024 || xml.find("<!DOCTYPE") != std::string::npos ||
        doc.Parse(xml.c_str(), xml.size()) != tinyxml2::XML_SUCCESS || !doc.RootElement())
        throw std::runtime_error("Invalid or oversized Sonos XML response");
}
int number(const std::string& s, int max = INT_MAX) {
    if (s.empty() || s.size() > 10) throw std::runtime_error("Missing or invalid Sonos number");
    long long n = 0;
    for (unsigned char c : s) {
        if (!std::isdigit(c)) throw std::runtime_error("Invalid Sonos number");
        n = n * 10 + c - '0';
        if (n > max) throw std::runtime_error("Sonos number out of range");
    }
    return static_cast<int>(n);
}
std::string sid(const std::string& uri) {
    auto q = uri.find('?');
    if (q == std::string::npos) return "";
    size_t start = q + 1;
    while (start < uri.size()) {
        auto end = uri.find('&', start);
        auto pair = uri.substr(start, end == std::string::npos ? end : end - start);
        if (pair.rfind("sid=", 0) == 0) {
            try { return std::to_string(number(pair.substr(4), 65535)); } catch (...) { return ""; }
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return "";
}
std::string provider(const std::string& uri, const std::string& metadata, const Fields& services) {
    std::set<std::string> identities;
    auto uri_sid = sid(uri);
    if (!uri_sid.empty()) identities.insert(uri_sid);
    tinyxml2::XMLDocument doc;
    parse(doc, metadata);
    // A descriptor carries serviceType = serviceId * 256 + 7.
    auto desc = descendant(doc.RootElement(), "desc");
    auto value = text(desc);
    const std::string prefix = "SA_RINCON";
    if (value.rfind(prefix, 0) == 0) {
        auto end = value.find('_', prefix.size());
        try {
            int type = number(value.substr(prefix.size(), end - prefix.size()));
            if (type % 256 != 7) return "";
            identities.insert(std::to_string(type / 256));
        } catch (...) { return ""; }
    }
    if (identities.size() != 1) return "";
    auto it = services.find(*identities.begin());
    if (it == services.end()) return "";
    return it->second == "Apple Music" || it->second == "Sonos Radio" ? it->second : "";
}
}

bool valid_ipv4(const std::string& ip) {
    if (ip.empty() || ip.size() > 15) return false;
    size_t start = 0;
    for (int i = 0; i < 4; ++i) {
        auto end = ip.find('.', start);
        if ((i < 3 && end == std::string::npos) || (i == 3 && end != std::string::npos)) return false;
        auto part = ip.substr(start, end == std::string::npos ? end : end - start);
        try { number(part, 255); } catch (...) { return false; }
        start = end + 1;
    }
    return true;
}
int parse_clock(const std::string& hms) {
    int total = 0, parts = 0;
    size_t start = 0;
    while (start <= hms.size()) {
        auto end = hms.find(':', start);
        auto part = hms.substr(start, end == std::string::npos ? end : end - start);
        try { total = total * 60 + number(part, 1000000); } catch (...) { return -1; }
        ++parts;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return parts >= 2 && parts <= 3 ? total : -1;  // NOT_IMPLEMENTED and "" fail here
}
std::string ipv4_from_url(const std::string& url) {
    if (url.rfind("http://", 0) != 0) return "";
    auto end = url.find_first_of(":/", 7);
    auto ip = url.substr(7, end == std::string::npos ? end : end - 7);
    return valid_ipv4(ip) ? ip : "";
}
std::string escape(const std::string& value) {
    std::string result;
    for (char c : value) switch (c) {
        case '&': result += "&amp;"; break;
        case '<': result += "&lt;"; break;
        case '>': result += "&gt;"; break;
        case '"': result += "&quot;"; break;
        case '\'': result += "&apos;"; break;
        default: result += c;
    }
    return result;
}
Fields parse_response(const std::string& xml) {
    tinyxml2::XMLDocument doc;
    parse(doc, xml);
    auto body = child(doc.RootElement(), "Body");
    auto response = body ? body->FirstChildElement() : nullptr;
    if (!response) throw std::runtime_error("Missing SOAP response");
    if (local(response->Name()) == "Fault")
        throw std::runtime_error("Sonos error " + text(descendant(response, "errorCode")) + ": " + text(descendant(response, "errorDescription")));
    Fields values;
    for (auto e = response->FirstChildElement(); e; e = e->NextSiblingElement()) values[local(e->Name())] = text(e);
    return values;
}
std::vector<Room> parse_rooms(const std::string& xml) {
    tinyxml2::XMLDocument doc;
    parse(doc, xml);
    auto groups = descendant(doc.RootElement(), "ZoneGroups");
    if (!groups) throw std::runtime_error("Missing Sonos room topology");
    std::vector<Room> rooms;
    for (auto group = groups->FirstChildElement(); group; group = group->NextSiblingElement()) {
        auto coordinator = attr(group, "Coordinator");
        for (auto member = group->FirstChildElement(); member; member = member->NextSiblingElement()) {
            if (local(member->Name()) != "ZoneGroupMember" || attr(member, "Invisible") == "1") continue;
            auto ip = ipv4_from_url(attr(member, "Location"));
            auto id = attr(member, "UUID");
            if (!ip.empty() && !id.empty()) rooms.push_back({id, attr(member, "ZoneName"), ip, coordinator});
        }
    }
    std::sort(rooms.begin(), rooms.end(), [](const Room& a, const Room& b) { return a.name < b.name; });
    return rooms;
}
Fields parse_services(const std::string& xml) {
    tinyxml2::XMLDocument doc;
    parse(doc, xml);
    Fields services;
    for (auto e = doc.RootElement()->FirstChildElement(); e; e = e->NextSiblingElement()) {
        if (local(e->Name()) != "Service") continue;
        auto name = attr(e, "Name");
        if (name == "Apple Music" || name == "Sonos Radio") services[attr(e, "Id")] = name;
    }
    return services;
}
std::vector<Favorite> parse_favorites(const std::string& xml, const Fields& services) {
    tinyxml2::XMLDocument doc;
    parse(doc, xml);
    std::vector<Favorite> favorites;
    for (auto item = doc.RootElement()->FirstChildElement(); item; item = item->NextSiblingElement()) {
        Favorite f;
        f.id = attr(item, "id"); f.title = text(child(item, "title"));
        f.uri = text(child(item, "res")); f.metadata = text(child(item, "resMD"));
        f.art = text(child(item, "albumArtURI"));
        if (f.uri.empty() || f.metadata.empty()) continue;
        try { f.provider = provider(f.uri, f.metadata, services); } catch (...) { continue; }
        if (f.provider.empty()) continue;
        tinyxml2::XMLDocument md;
        parse(md, f.metadata);
        auto klass = text(descendant(md.RootElement(), "class"));
        f.radio = klass.find("audioBroadcast") != std::string::npos ||
                  f.uri.rfind("x-sonosapi-stream:", 0) == 0 || f.uri.rfind("x-sonosapi-radio:", 0) == 0 ||
                  f.uri.rfind("x-sonosapi-hls:", 0) == 0;
        favorites.push_back(std::move(f));
    }
    return favorites;
}
std::vector<QueueItem> parse_queue(const std::string& didl, int first_number) {
    tinyxml2::XMLDocument doc;
    parse(doc, didl);
    std::vector<QueueItem> items;
    for (auto item = doc.RootElement()->FirstChildElement(); item; item = item->NextSiblingElement()) {
        if (local(item->Name()) != "item") continue;
        QueueItem q;
        q.number = first_number++;
        q.title = text(child(item, "title"));
        q.artist = text(child(item, "creator"));
        if (q.artist.empty()) q.artist = text(child(item, "artist"));
        q.album = text(child(item, "album"));
        q.art = text(child(item, "albumArtURI"));
        items.push_back(std::move(q));
    }
    return items;
}
Fields Client::call(const std::string& ip, const std::string& service, const std::string& action, const Fields& fields) {
    if (!valid_ipv4(ip)) throw std::runtime_error("Invalid speaker address");
    std::string path, urn;
    if (service == "AVTransport" || service == "RenderingControl" || service == "GroupRenderingControl") path = "/MediaRenderer/";
    else if (service == "ContentDirectory") path = "/MediaServer/";
    else path = "/";
    path += service + "/Control";
    const bool rincon = service == "ZoneGroupTopology" || service == "MusicServices";
    urn = std::string("urn:") + (rincon ? "schemas-rinconnetworks-com" : "schemas-upnp-org") + ":service:" + service + ":1";
    std::string body = "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:" + action + " xmlns:u=\"" + urn + "\">";
    // UPnP devices can require argument order; maps are not used for serialization order.
    const std::vector<std::string> order = {"InstanceID","Channel","ObjectID","BrowseFlag","Filter","StartingIndex","RequestedCount","SortCriteria","CurrentURI","CurrentURIMetaData","EnqueuedURI","EnqueuedURIMetaData","DesiredFirstTrackNumberEnqueued","EnqueueAsNext","Unit","Target","Speed","DesiredVolume","DesiredMute","NewCoordinator","RejoinGroup"};
    size_t count = 0;
    for (const auto& key : order) if (auto it = fields.find(key); it != fields.end()) {
        body += "<" + key + ">" + escape(it->second) + "</" + key + ">";
        ++count;
    }
    if (count != fields.size()) throw std::runtime_error("Unsupported SOAP argument");
    body += "</u:" + action + "></s:Body></s:Envelope>";
    return parse_response(transport_({ip, path, urn, action, body}));
}
std::vector<Room> Client::rooms(const std::string& seed) {
    topology_ = parse_rooms(call(seed, "ZoneGroupTopology", "GetZoneGroupState").at("ZoneGroupState"));
    return topology_;
}
Room Client::coordinator(const Room& room) {
    auto find = [](const std::vector<Room>& all, const std::string& id) {
        return std::find_if(all.begin(), all.end(), [&](const Room& r) { return r.id == id; });
    };
    if (find(topology_, room.id) == topology_.end()) rooms(room.ip);
    auto selected = find(topology_, room.id);
    if (selected == topology_.end()) throw std::runtime_error("Room is no longer available");
    auto leader = find(topology_, selected->coordinator);
    if (leader == topology_.end()) throw std::runtime_error("Group coordinator is unavailable");
    return *leader;
}
std::vector<Favorite> Client::favorites(const std::string& seed) {
    services_ = parse_services(call(seed,"MusicServices","ListAvailableServices").at("AvailableServiceDescriptorList"));
    std::vector<Favorite> result;
    std::string update;
    for (int start = 0; start < 1000;) {
        if (start && interrupt_ && interrupt_()) throw Preempted();
        auto response = call(seed,"ContentDirectory","Browse",{{"ObjectID","FV:2"},{"BrowseFlag","BrowseDirectChildren"},{"Filter","*"},{"StartingIndex",std::to_string(start)},{"RequestedCount","64"},{"SortCriteria",""}});
        if (start && response.at("UpdateID") != update) throw std::runtime_error("Favorites changed; refresh again");
        update = response.at("UpdateID");
        auto page = parse_favorites(response.at("Result"), services_);
        result.insert(result.end(),page.begin(),page.end());
        int returned = number(response.at("NumberReturned"),64), total = number(response.at("TotalMatches"));
        if (!returned && start < total) throw std::runtime_error("Incomplete favorites response");
        start += returned;
        if (start >= total) return result;
    }
    throw std::runtime_error("Favorites exceed the 1000-item device limit");
}
std::vector<QueueItem> Client::queue(const Room& room, int start, int count, int* total) {
    if (start < 0 || count < 1) throw std::runtime_error("Invalid queue range");
    auto target = coordinator(room);
    auto response = call(target.ip,"ContentDirectory","Browse",
        {{"ObjectID","Q:0"},{"BrowseFlag","BrowseDirectChildren"},
         {"Filter","dc:title,res,dc:creator,upnp:artist,upnp:album,upnp:albumArtURI"},
         {"StartingIndex",std::to_string(start)},{"RequestedCount",std::to_string(std::min(count,100))},{"SortCriteria",""}});
    if (total) *total = number(response.at("TotalMatches"));
    return parse_queue(response.at("Result"), start + 1);
}
std::string source_title(const std::string& track_uri) {
    if (track_uri.rfind("x-sonos-htastream:",0)==0) return "TV";
    if (track_uri.rfind("x-rincon-stream:",0)==0) return "Line-In";
    return "";
}
State Client::state(const Room& room) {
    auto target = coordinator(room);
    auto hold = [&] { if (interrupt_ && interrupt_()) throw Preempted(); };
    auto p = call(target.ip,"AVTransport","GetPositionInfo",{{"InstanceID","0"}});
    hold();
    auto t = call(target.ip,"AVTransport","GetTransportInfo",{{"InstanceID","0"}});
    hold();
    auto a = call(target.ip,"AVTransport","GetCurrentTransportActions",{{"InstanceID","0"}});
    hold();
    auto v = call(room.ip,"RenderingControl","GetVolume",{{"InstanceID","0"},{"Channel","Master"}});
    hold();
    auto m = call(room.ip,"RenderingControl","GetMute",{{"InstanceID","0"},{"Channel","Master"}});
    hold();
    auto g = call(target.ip,"GroupRenderingControl","GetGroupVolume",{{"InstanceID","0"}});
    hold();
    auto gm = call(target.ip,"GroupRenderingControl","GetGroupMute",{{"InstanceID","0"}});
    State s;
    s.playback=t["CurrentTransportState"]; s.actions=a["Actions"];
    s.volume=number(v.at("CurrentVolume"),100); s.muted=m["CurrentMute"]=="1";
    s.group_volume=number(g.at("CurrentVolume"),100); s.group_muted=gm["CurrentMute"]=="1";
    try { s.track=p["Track"].empty() ? 0 : number(p["Track"]); } catch (...) { s.track=0; } // e.g. NOT_IMPLEMENTED
    s.position=parse_clock(p["RelTime"]);
    s.duration=parse_clock(p["TrackDuration"]);
    if (s.duration <= 0) s.position = s.duration = -1;  // streams report 0:00:00
    if (!p["TrackMetaData"].empty() && p["TrackMetaData"]!="NOT_IMPLEMENTED") {
        tinyxml2::XMLDocument doc; parse(doc,p["TrackMetaData"]);
        s.title=text(descendant(doc.RootElement(),"title"));
        s.artist=text(descendant(doc.RootElement(),"creator"));
        s.album=text(descendant(doc.RootElement(),"album"));
        s.art=text(descendant(doc.RootElement(),"albumArtURI"));
    }
    if (s.title.empty()) s.title=source_title(p["TrackURI"]);
    return s;
}
Summary Client::summary(const Room& coordinator) {
    auto p = call(coordinator.ip,"AVTransport","GetPositionInfo",{{"InstanceID","0"}});
    auto t = call(coordinator.ip,"AVTransport","GetTransportInfo",{{"InstanceID","0"}});
    Summary s; s.playback = t["CurrentTransportState"];
    if (!p["TrackMetaData"].empty() && p["TrackMetaData"]!="NOT_IMPLEMENTED") {
        tinyxml2::XMLDocument doc; parse(doc,p["TrackMetaData"]);
        s.title=text(descendant(doc.RootElement(),"title"));
        s.artist=text(descendant(doc.RootElement(),"creator"));
    }
    if (s.title.empty()) s.title=source_title(p["TrackURI"]);
    return s;
}
void Client::play_favorite(const Room& room, const Favorite& favorite) {
    if (provider(favorite.uri, favorite.metadata, services_).empty()) throw std::runtime_error("Only Apple Music and Sonos Radio favorites can be played");
    auto target=coordinator(room);
    if (favorite.radio) call(target.ip,"AVTransport","SetAVTransportURI",{{"InstanceID","0"},{"CurrentURI",favorite.uri},{"CurrentURIMetaData",favorite.metadata}});
    else {
        // Replace the queue, matching the official app's "Replace Queue".
        call(target.ip,"AVTransport","RemoveAllTracksFromQueue",{{"InstanceID","0"}});
        auto added=call(target.ip,"AVTransport","AddURIToQueue",{{"InstanceID","0"},{"EnqueuedURI",favorite.uri},{"EnqueuedURIMetaData",favorite.metadata},{"DesiredFirstTrackNumberEnqueued","0"},{"EnqueueAsNext","0"}});
        auto first=std::to_string(number(added.at("FirstTrackNumberEnqueued")));
        call(target.ip,"AVTransport","SetAVTransportURI",{{"InstanceID","0"},{"CurrentURI","x-rincon-queue:"+target.id+"#0"},{"CurrentURIMetaData",""}});
        call(target.ip,"AVTransport","Seek",{{"InstanceID","0"},{"Unit","TRACK_NR"},{"Target",first}});
    }
    call(target.ip,"AVTransport","Play",{{"InstanceID","0"},{"Speed","1"}});
}
void Client::play_queue_track(const Room& room, int number) {
    if (number < 1) throw std::runtime_error("Invalid queue track number");
    auto target=coordinator(room);
    const auto uri="x-rincon-queue:"+target.id+"#0";
    // If the source is already this queue, seeking alone avoids reloading it.
    if (call(target.ip,"AVTransport","GetMediaInfo",{{"InstanceID","0"}})["CurrentURI"]!=uri)
        call(target.ip,"AVTransport","SetAVTransportURI",{{"InstanceID","0"},{"CurrentURI",uri},{"CurrentURIMetaData",""}});
    call(target.ip,"AVTransport","Seek",{{"InstanceID","0"},{"Unit","TRACK_NR"},{"Target",std::to_string(number)}});
    call(target.ip,"AVTransport","Play",{{"InstanceID","0"},{"Speed","1"}});
}
void Client::transport(const Room& room, const std::string& action) {
    if (action!="Play" && action!="Pause" && action!="Stop" && action!="Next" && action!="Previous") throw std::runtime_error("Unsupported playback action");
    auto target=coordinator(room);
    Fields args{{"InstanceID","0"}};
    if(action=="Play") args["Speed"]="1";
    call(target.ip,"AVTransport",action,args);
}
void Client::volume(const Room& room, int value, bool group) {
    if(value<0 || value>100) throw std::runtime_error("Volume must be 0 to 100");
    if(group) {
        auto target=coordinator(room);
        call(target.ip,"GroupRenderingControl","SnapshotGroupVolume",{{"InstanceID","0"}});
        call(target.ip,"GroupRenderingControl","SetGroupVolume",{{"InstanceID","0"},{"DesiredVolume",std::to_string(value)}});
    }
    else call(room.ip,"RenderingControl","SetVolume",{{"InstanceID","0"},{"Channel","Master"},{"DesiredVolume",std::to_string(value)}});
}
void Client::mute(const Room& room, bool value, bool group) {
    if(group) call(coordinator(room).ip,"GroupRenderingControl","SetGroupMute",{{"InstanceID","0"},{"DesiredMute",value?"1":"0"}});
    else call(room.ip,"RenderingControl","SetMute",{{"InstanceID","0"},{"Channel","Master"},{"DesiredMute",value?"1":"0"}});
}
void Client::join(const Room& room, const Room& destination) {
    auto target=coordinator(destination);
    if(room.id==target.id) return;
    invalidate_topology();
    call(room.ip,"AVTransport","SetAVTransportURI",{{"InstanceID","0"},{"CurrentURI","x-rincon:"+target.id},{"CurrentURIMetaData",""}});
}
void Client::ungroup(const Room& room) {
    invalidate_topology();
    call(room.ip,"AVTransport","BecomeCoordinatorOfStandaloneGroup",{{"InstanceID","0"}});
}
void Client::leave(const Room& room) {
    const auto all=rooms(room.ip);
    auto self=std::find_if(all.begin(),all.end(),[&](const Room& r){return r.id==room.id;});
    if(self==all.end()) throw std::runtime_error("Room is no longer available");
    auto heir=std::find_if(all.begin(),all.end(),[&](const Room& r){return r.id!=self->id && r.coordinator==self->coordinator;});
    if(heir==all.end()) return;  // alone already
    if(self->coordinator==self->id) {
        try {
            invalidate_topology();
            call(self->ip,"AVTransport","DelegateGroupCoordinationTo",{{"InstanceID","0"},{"NewCoordinator",heir->id},{"RejoinGroup","0"}});
            return;
        } catch (const std::exception&) {}  // older firmware: fall back to splitting off
    }
    ungroup(*self);
}
Level Client::level(const Room& room) {
    auto v=call(room.ip,"RenderingControl","GetVolume",{{"InstanceID","0"},{"Channel","Master"}});
    auto m=call(room.ip,"RenderingControl","GetMute",{{"InstanceID","0"},{"Channel","Master"}});
    return {number(v.at("CurrentVolume"),100), m["CurrentMute"]=="1"};
}
void Client::apply_area(const std::string& seed, const std::vector<std::string>& ids) {
    if(ids.empty() || ids.size()>32) throw std::runtime_error("Invalid saved area");
    auto all=rooms(seed);
    std::vector<Room> wanted;
    std::set<std::string> unique;
    for(const auto& id:ids) {
        if(!unique.insert(id).second) continue;
        auto it=std::find_if(all.begin(),all.end(),[&](const Room& r){return r.id==id;});
        if(it==all.end()) throw std::runtime_error("An area room is unavailable; grouping was not changed");
        wanted.push_back(*it);
    }
    auto base=wanted.front();
    const auto group=base.coordinator;
    bool outsiders=std::any_of(all.begin(),all.end(),[&](const Room& r){return r.coordinator==group && !unique.count(r.id);});
    if(outsiders) ungroup(base);
    else {
        auto leader=std::find_if(wanted.begin(),wanted.end(),[&](const Room& r){return r.id==group;});
        if(leader!=wanted.end()) base=*leader;
    }
    // Rooms already in the kept group need no join; re-sending one can drop audio.
    for(const auto& r:wanted) if(r.id!=base.id && (outsiders || r.coordinator!=group)) join(r,base);
}
}
