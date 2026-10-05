#include "ui.hpp"
#include "screen.hpp"
#include "network.hpp"
#include "bsp/esp-bsp.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "misc/cache/instance/lv_image_cache.h"
#include <algorithm>
#include <functional>
#include <map>
#include <string_view>

LV_FONT_DECLARE(font_title_56)
LV_FONT_DECLARE(font_title_36)
LV_FONT_DECLARE(font_body_36)
LV_FONT_DECLARE(font_body_32)
LV_FONT_DECLARE(font_body_26)
LV_FONT_DECLARE(font_caption_22)
LV_FONT_DECLARE(font_icons_52)

namespace {
// Design tokens. The panel is ~11.6 px/mm and read from arm's length, so the
// smallest tap target is 88 px (~7.5 mm) and body text is 26 px or larger.
namespace ink {
constexpr uint32_t bg=0x0F1216, surface=0x1A1F25, raised=0x242B33, pressed=0x323B45;
constexpr uint32_t text=0xF2F4F6, quiet=0xB4BBC3, muted=0xA3ADB8, faint=0x6E7883, accent=0xF5A524, on_accent=0x1C1405, danger=0xFF6B6B;
}
constexpr int RAIL=128, HEADER=96, W=1280, H=720, CONTENT_W=W-RAIL, CONTENT_H=H-HEADER, PAD=40, TARGET=88;
const char* ICON_STAR="\xEF\x80\x85";
const char* ICON_LINK="\xEF\x83\x81";
const char* ICON_RADIO="\xEF\x94\x99";

struct DisplayLock { DisplayLock(){bsp_display_lock(0);} ~DisplayLock(){bsp_display_unlock();} };
lv_color_t c(uint32_t hex) { return lv_color_hex(hex); }

// ---- small builders ---------------------------------------------------------
lv_obj_t* div(lv_obj_t* parent) {
    auto o=lv_obj_create(parent); lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o,LV_OBJ_FLAG_CLICKABLE); lv_obj_remove_flag(o,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(o,LV_OBJ_FLAG_EVENT_BUBBLE);
    return o;
}
lv_obj_t* column(lv_obj_t* parent,int gap) {
    auto o=div(parent); lv_obj_set_flex_flow(o,LV_FLEX_FLOW_COLUMN); lv_obj_set_style_pad_row(o,gap,0); return o;
}
lv_obj_t* row(lv_obj_t* parent,int gap) {
    auto o=div(parent); lv_obj_set_flex_flow(o,LV_FLEX_FLOW_ROW); lv_obj_set_style_pad_column(o,gap,0);
    lv_obj_set_flex_align(o,LV_FLEX_ALIGN_START,LV_FLEX_ALIGN_CENTER,LV_FLEX_ALIGN_CENTER); return o;
}
lv_obj_t* text(lv_obj_t* parent,const lv_font_t* font,uint32_t color,const char* s="") {
    auto l=lv_label_create(parent); lv_label_set_text(l,s);
    lv_obj_set_style_text_font(l,font,0); lv_obj_set_style_text_color(l,c(color),0);
    lv_obj_add_flag(l,LV_OBJ_FLAG_EVENT_BUBBLE);
    return l;
}
// Truncates with an ellipsis; LVGL only truncates when the height is fixed, otherwise it wraps.
lv_obj_t* one_line(lv_obj_t* l,int width) {
    lv_obj_set_size(l,width,lv_font_get_line_height(lv_obj_get_style_text_font(l,LV_PART_MAIN)));
    lv_label_set_long_mode(l,LV_LABEL_LONG_DOT); return l;
}
// A plain tappable surface: filled, rounded, darker/lighter when pressed.
lv_obj_t* tappable(lv_obj_t* parent,int w,int h,uint32_t bg,uint32_t pressed,int radius,lv_event_cb_t cb,void* data=nullptr) {
    auto b=lv_button_create(parent); lv_obj_remove_style_all(b);
    lv_obj_set_size(b,w,h);
    lv_obj_set_style_bg_opa(b,LV_OPA_COVER,0);
    lv_obj_set_style_bg_color(b,c(bg),0); lv_obj_set_style_bg_color(b,c(pressed),LV_STATE_PRESSED);
    lv_obj_set_style_radius(b,radius,0);
    lv_obj_set_style_opa(b,LV_OPA_40,LV_STATE_DISABLED);
    lv_obj_remove_flag(b,LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    if(cb) lv_obj_add_event_cb(b,cb,LV_EVENT_CLICKED,data);
    return b;
}
lv_obj_t* icon_button(lv_obj_t* parent,const char* icon,int size,bool primary,lv_event_cb_t cb,void* data=nullptr) {
    auto b=tappable(parent,size,size,primary?ink::accent:ink::raised,primary?0xFFC45C:ink::pressed,LV_RADIUS_CIRCLE,cb,data);
    auto l=text(b,size>=112?&font_icons_52:&font_body_32,primary?ink::on_accent:ink::text,icon); lv_obj_center(l);
    return b;
}
lv_obj_t* pill_button(lv_obj_t* parent,const char* label,bool primary,lv_event_cb_t cb,void* data=nullptr) {
    auto b=tappable(parent,LV_SIZE_CONTENT,TARGET,primary?ink::accent:ink::raised,primary?0xFFC45C:ink::pressed,TARGET/2,cb,data);
    lv_obj_set_style_pad_hor(b,36,0);
    auto l=text(b,&font_body_26,primary?ink::on_accent:ink::text,label); lv_obj_center(l);
    return b;
}
void set_icon(lv_obj_t* button,const char* icon) { lv_label_set_text(lv_obj_get_child(button,0),icon); }
// Wraps decoded pixels for an image widget. The descriptor must outlive the widget's use of it.
void show_image(lv_obj_t* image,lv_image_dsc_t& dsc,const Artwork& art) {
    lv_image_set_src(image,nullptr);
    lv_image_cache_drop(&dsc);
    if(!art.pixels) return;
    dsc=lv_image_dsc_t{};
    dsc.header.magic=LV_IMAGE_HEADER_MAGIC; dsc.header.cf=LV_COLOR_FORMAT_RGB565;
    dsc.header.w=art.width; dsc.header.h=art.height; dsc.header.stride=art.stride;
    dsc.data_size=art.stride*art.height; dsc.data=art.pixels;
    lv_image_set_src(image,&dsc);
}
// An artwork frame: rounded surface with a placeholder icon and an image scaled to fill.
struct ArtFrame {
    lv_obj_t *frame=nullptr,*placeholder=nullptr,*image=nullptr;
    Artwork art; lv_image_dsc_t dsc{}; int side=0;
    void build(lv_obj_t* parent,int size,int radius,const char* icon) {
        side=size;
        frame=div(parent); lv_obj_set_size(frame,size,size);
        lv_obj_set_style_bg_opa(frame,LV_OPA_COVER,0); lv_obj_set_style_bg_color(frame,c(ink::raised),0);
        lv_obj_set_style_radius(frame,radius,0);  // no clip_corner: covers arrive pre-rounded
        placeholder=text(frame,size>=240?&font_icons_52:&font_body_32,ink::faint,icon); lv_obj_center(placeholder);
        image=lv_image_create(frame); lv_obj_center(image); lv_obj_add_flag(image,LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_add_flag(image,LV_OBJ_FLAG_HIDDEN);
    }
    void set(Artwork next) {
        lv_image_set_src(image,nullptr);  // detach the old pixels before they are freed
        lv_image_cache_drop(&dsc);
        art=std::move(next);
        if(art.pixels) show_image(image,dsc,art);
        const bool shown=art.pixels!=nullptr;
        if(shown) lv_obj_center(image);  // arrives at exactly side x side, drawn unscaled
        // The cover hides the frame entirely; drawing its fill underneath is wasted work.
        lv_obj_set_style_bg_opa(frame,shown?LV_OPA_TRANSP:LV_OPA_COVER,0);
        lv_obj_set_flag(image,LV_OBJ_FLAG_HIDDEN,!shown);
        lv_obj_set_flag(placeholder,LV_OBJ_FLAG_HIDDEN,shown);
    }
};
std::string clock_text(int seconds) {
    if(seconds<0) return "";
    char buf[16];
    if(seconds>=3600) snprintf(buf,sizeof buf,"%d:%02d:%02d",seconds/3600,seconds/60%60,seconds%60);
    else snprintf(buf,sizeof buf,"%d:%02d",seconds/60,seconds%60);
    return buf;
}

// ---- model (touched only with the display lock held) -------------------------
std::vector<sonos::Room> rooms;
std::vector<sonos::Favorite> favorites;
std::map<std::string,sonos::Summary> summaries;
sonos::Room selected;
sonos::State current; bool have_state=false;
View view=View::NowPlaying;
Battery battery; bool battery_known=false, online=false;
// Track position is interpolated between polls so the progress bar moves smoothly.
int64_t position_at=0;

std::vector<const sonos::Room*> group_of(const sonos::Room& room) {
    std::vector<const sonos::Room*> members;
    for(auto& r:rooms) if(!room.coordinator.empty() && r.coordinator==room.coordinator) members.push_back(&r);
    return members;
}
void send(Command* command) { if(!submit(command)) ui_toast("Still working on the last request. Try again.",true); }
Command* for_room(const char* action) { auto k=new Command; k->action=action; k->room=selected; return k; }

// ---- widgets ------------------------------------------------------------------
lv_obj_t *rail_items[5], *views[5];
lv_obj_t *battery_label,*offline_label,*toast,*keyboard;
lv_timer_t* toast_timer;
// Now Playing
ArtFrame now_art;
lv_obj_t *np_title,*np_artist,*np_progress,*np_elapsed,*np_remaining,*np_times;
lv_obj_t *np_prev,*np_play,*np_next,*np_mute,*np_volume,*np_volume_value,*np_volume_caption;
lv_obj_t *np_full,*np_empty,*np_empty_title,*np_empty_button;  // playing layout vs. centered empty state
// Favorites
lv_obj_t* fav_grid,*fav_hint;
// Each favorite tile is one pre-composed image: cover (or placeholder), title
// and radio badge drawn once onto a canvas. As separate widgets (card, frame,
// image, title, badge) the ~90 objects cost ~13 ms per scroll frame in tree
// walks and per-object draw events, plus time moving them on every scroll step.
constexpr int32_t TILE_W=art_spec::tile_side, TILE_TITLE_Y=art_spec::tile_side+8, TILE_H=TILE_TITLE_Y+72;
// The grid's column gap (applied as the grid's pad_column). An even stride keeps
// each tile's destination offset 4-byte aligned, and five columns must fit.
constexpr int32_t FAV_COLUMNS=5, FAV_GAP=22;
static_assert((TILE_W+FAV_GAP)%2==0 && PAD%2==0,"tile x positions must stay even");
static_assert(FAV_COLUMNS*TILE_W+(FAV_COLUMNS-1)*FAV_GAP<=CONTENT_W-2*PAD,"five favorite tiles must fit a row");
// Every favorite gets a canvas; the ~106 KB draw buffer behind it is made when
// the tile first nears the viewport and then kept, up to TILE_BUDGET_BYTES.
// Only past that budget (the accepted 1,000-item catalog would need over
// 100 MiB) are the buffers farthest from the viewport released, and their
// covers fetched again if they come back. The decoded cover is not kept: once
// drawn into the tile it would only duplicate those pixels.
constexpr size_t TILE_BUDGET_BYTES=16u<<20;  // of 32 MiB PSRAM; ~27 MiB is free with a full screen of covers
constexpr size_t TILE_BUFFER_LIMIT=TILE_BUDGET_BYTES/(TILE_W*2*TILE_H);
struct Tile {
    lv_obj_t* canvas=nullptr;
    lv_draw_buf_t* pixels=nullptr;   // from first approach until evicted by the budget
    std::string title;               // wrapped and "..."-truncated by a label once
    bool radio=false, have_art=false;
    int64_t art_asked=0;             // esp_timer time of the last art request
    Tile()=default;
    Tile(Tile&& o) noexcept { *this=std::move(o); }
    Tile& operator=(Tile&& o) noexcept {
        if(this!=&o) {
            canvas=o.canvas; pixels=o.pixels; title=std::move(o.title);
            radio=o.radio; have_art=o.have_art; art_asked=o.art_asked; o.pixels=nullptr;
        }
        return *this;
    }
    Tile(const Tile&)=delete;
    Tile& operator=(const Tile&)=delete;
    ~Tile() { if(pixels) lv_draw_buf_destroy(pixels); }
};
std::vector<Tile> tiles;
// Queue, Rooms, Settings
lv_obj_t *queue_list,*queue_header;
lv_obj_t* room_grid;
// One card per room, built when the room list changes and otherwise updated in
// place: rebuilding on every tap or poll deleted cards under a finger and
// dropped quick taps.
struct RoomCard { lv_obj_t *card=nullptr,*name=nullptr,*detail=nullptr,*mates=nullptr,*badge=nullptr,*slider=nullptr; };
std::vector<RoomCard> room_cards;
std::map<std::string,sonos::Level> levels;  // each room's own volume, when known
// Join/Leave sent but not yet seen in the topology: the card shows the
// intended state until it is, or for 10 s if the speakers never confirm it.
struct Pending { bool join; int64_t at; };
std::map<std::string,Pending> pending;
lv_obj_t *ssid_input,*password_input,*seed_input,*device_info;
// Settings shows the saved network; Edit asks for the passcode, then opens the form.
enum class SettingsMode { Summary, Passcode, Form };
lv_obj_t *settings_summary,*settings_passcode,*settings_form,*ssid_value,*pin_dots[4];
std::string pin_entry;

constexpr lv_opa_t RAIL_ACTIVE_OPA=0x3D;  // the active tab is tinted with the accent at 24%
void render_rail() {
    for(int i=0;i<5;++i) {
        const bool active=static_cast<int>(view)==i;
        lv_obj_set_style_bg_opa(rail_items[i],active?RAIL_ACTIVE_OPA:LV_OPA_TRANSP,0);
        for(uint32_t k=0;k<lv_obj_get_child_count(rail_items[i]);++k)
            lv_obj_set_style_text_color(lv_obj_get_child(rail_items[i],k),c(active?ink::accent:ink::muted),0);
    }
}
void render_device_info();
void show_settings(SettingsMode mode);
void render_rooms();
void render_now_playing();
void update_tile_cache();

void show_view(View next) {
    // Queue and Rooms load on demand; a busy worker picks them up on its next poll.
    if(next!=view && (next==View::Queue || next==View::Rooms)) submit(for_room(next==View::Queue?"Queue":"Poll"));
    if(next==View::Favorites) update_tile_cache();
    view=next;
    for(int i=0;i<5;++i) lv_obj_set_flag(views[i],LV_OBJ_FLAG_HIDDEN,i!=static_cast<int>(next));
    lv_obj_add_flag(keyboard,LV_OBJ_FLAG_HIDDEN);
    render_rail();
    // Settings always reopens on the summary; with no network saved yet there
    // is nothing to protect, so it goes straight to the form.
    if(next==View::Settings) { render_device_info(); show_settings(setting("ssid").empty()?SettingsMode::Form:SettingsMode::Summary); }
    if(next==View::Rooms) render_rooms();
}
void nav_clicked(lv_event_t* e) {
    const auto next=static_cast<View>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(e)));
    show_view(next);
}

// ---- Now Playing ------------------------------------------------------------------
void transport_clicked(lv_event_t* e) {
    if(selected.id.empty()) return;
    std::string action=static_cast<const char*>(lv_event_get_user_data(e));
    auto supports=[](const char* a){ return (","+current.actions+",").find(std::string(",")+a+",")!=std::string::npos; };
    if(action=="Toggle") {
        if(current.playback=="PLAYING") action=supports("Pause")?"Pause":"Stop";
        else action="Play";
    }
    // Reflect the press immediately; the next poll confirms it.
    if(action=="Pause"||action=="Stop") { current.playback="PAUSED_PLAYBACK"; set_icon(np_play,LV_SYMBOL_PLAY); }
    if(action=="Play") { current.playback="PLAYING"; set_icon(np_play,LV_SYMBOL_PAUSE); }
    send(for_room(action.c_str()));
}
bool grouped() { return group_of(selected).size()>1; }
bool muted_now() { return grouped()?current.group_muted:current.muted; }
// Muted reads as zero on every volume bar; unmuting puts the real level back.
int shown_volume() { return muted_now()?0:grouped()?current.group_volume:current.volume; }
int shown_level(const sonos::Level& l) { return l.muted?0:l.volume; }
bool any_slider_pressed() {
    if(lv_obj_has_state(np_volume,LV_STATE_PRESSED)) return true;
    for(auto& rc:room_cards) if(lv_obj_has_state(rc.slider,LV_STATE_PRESSED)) return true;
    return false;
}
// Room cards: each selected room's own level; hidden until it is known, so a
// newly selected room never flashes an empty bar.
void render_card_sliders() {
    for(size_t i=0;i<room_cards.size() && i<rooms.size();++i) {
        auto slider=room_cards[i].slider;
        if(lv_obj_has_flag(slider,LV_OBJ_FLAG_HIDDEN) || lv_obj_has_state(slider,LV_STATE_PRESSED)) continue;
        if(auto it=levels.find(rooms[i].id); it!=levels.end()) lv_slider_set_value(slider,shown_level(it->second),LV_ANIM_OFF);
    }
}
void render_volume() {
    set_icon(np_mute,muted_now()?LV_SYMBOL_MUTE:LV_SYMBOL_VOLUME_MAX);
    if(any_slider_pressed()) return;
    const int level=shown_volume();
    lv_slider_set_value(np_volume,level,LV_ANIM_OFF);
    lv_label_set_text_fmt(np_volume_value,"%d",level);
    render_card_sliders();
}
void mute_clicked(lv_event_t*) {
    if(selected.id.empty()) return;
    const bool muted=muted_now();
    auto k=for_room(grouped()?"GroupMute":"Mute"); k->value=!muted; send(k);
    if(grouped()) {  // group mute mutes every member
        current.group_muted=!muted;
        for(auto* r:group_of(selected)) if(auto it=levels.find(r->id); it!=levels.end()) it->second.muted=!muted;
    } else {
        current.muted=!muted;
        if(auto it=levels.find(selected.id); it!=levels.end()) it->second.muted=!muted;
    }
    render_volume();
}
lv_obj_t* card_slider(const std::string& room_id) {
    for(size_t i=0;i<room_cards.size() && i<rooms.size();++i) if(rooms[i].id==room_id) return room_cards[i].slider;
    return nullptr;
}
// Now Playing's slider: the group's volume when grouped, else the room's (and
// then its card moves with it). Releasing sends the level.
void volume_event(lv_event_t* e) {
    const int value=lv_slider_get_value(np_volume);
    lv_label_set_text_fmt(np_volume_value,"%d",value);
    if(!grouped()) if(auto s=card_slider(selected.id)) lv_slider_set_value(s,value,LV_ANIM_OFF);
    if(lv_event_get_code(e)==LV_EVENT_RELEASED && !selected.id.empty()) {
        // Setting a level while muted unmutes, as in the Sonos app.
        auto k=for_room(grouped()?"GroupVolume":"Volume"); k->value=value; k->unmute=muted_now(); send(k);
        if(grouped()) { current.group_volume=value; current.group_muted=false; }
        else { current.volume=value; current.muted=false; levels[selected.id]={value,false}; }
        render_volume();
    }
}
// A room card's slider: that room's own volume, grouped or not.
void card_volume_event(lv_event_t* e) {
    const auto index=reinterpret_cast<uintptr_t>(lv_event_get_user_data(e));
    if(index>=rooms.size()) return;
    const auto room=rooms[index];
    const int value=lv_slider_get_value(room_cards[index].slider);
    const bool solo=room.id==selected.id && !grouped();
    if(solo) { lv_slider_set_value(np_volume,value,LV_ANIM_OFF); lv_label_set_text_fmt(np_volume_value,"%d",value); }
    if(lv_event_get_code(e)==LV_EVENT_RELEASED) {
        auto it=levels.find(room.id);
        auto k=new Command; k->action="Volume"; k->room=room; k->value=value; k->unmute=it!=levels.end() && it->second.muted; send(k);
        levels[room.id]={value,false};
        if(room.id==selected.id) { current.volume=value; current.muted=false; }
        render_volume();
    }
}
lv_obj_t* volume_slider(lv_obj_t* parent,int width,lv_event_cb_t cb=volume_event,void* data=nullptr) {
    // A thick bar-style slider: the whole 56 px track is the target, no thin knob.
    auto s=lv_slider_create(parent); lv_obj_set_size(s,width,56);
    lv_slider_set_range(s,0,100);
    lv_obj_set_style_bg_color(s,c(ink::raised),0); lv_obj_set_style_bg_opa(s,LV_OPA_COVER,0);
    lv_obj_set_style_radius(s,28,0); lv_obj_set_style_radius(s,28,LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s,c(ink::accent),LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s,LV_OPA_TRANSP,LV_PART_KNOB); lv_obj_set_style_pad_all(s,0,LV_PART_KNOB);
    lv_obj_set_style_shadow_width(s,0,LV_PART_KNOB);
    lv_obj_set_ext_click_area(s,16);
    lv_obj_add_event_cb(s,cb,LV_EVENT_VALUE_CHANGED,data);
    lv_obj_add_event_cb(s,cb,LV_EVENT_RELEASED,data);
    return s;
}
void empty_clicked(lv_event_t*) { show_view(selected.id.empty()?View::Rooms:View::Favorites); }
void render_progress() {
    const bool known=have_state && current.duration>0;
    lv_obj_set_flag(np_progress,LV_OBJ_FLAG_HIDDEN,!known);
    lv_obj_set_flag(np_times,LV_OBJ_FLAG_HIDDEN,!known);
    if(!known) return;
    int position=current.position;
    if(current.playback=="PLAYING") position+=static_cast<int>((esp_timer_get_time()-position_at)/1000000);
    position=std::clamp(position,0,current.duration);
    lv_bar_set_range(np_progress,0,current.duration);
    lv_bar_set_value(np_progress,position,LV_ANIM_OFF);
    lv_label_set_text(np_elapsed,clock_text(position).c_str());
    lv_label_set_text(np_remaining,("-"+clock_text(current.duration-position)).c_str());
}
// The title is 36 px SemiBold on 40 px lines, at most two of them.
int32_t title_line_space() { return 40-lv_font_get_line_height(&font_title_36); }
int32_t title_max_height() { return 2*lv_font_get_line_height(&font_title_36)+title_line_space(); }
void progress_tick(lv_timer_t*) { if(view==View::NowPlaying && !screen_off()) render_progress(); }
void render_now_playing() {
    const bool playing_something=have_state && !current.title.empty();
    lv_label_set_text(np_title,playing_something?current.title.c_str():"Nothing playing");
    // LVGL only truncates with "..." at a fixed height: measure, then cap at two lines.
    lv_obj_set_height(np_title,LV_SIZE_CONTENT); lv_obj_update_layout(np_title);
    if(lv_obj_get_height(np_title)>title_max_height()) lv_obj_set_height(np_title,title_max_height());
    lv_label_set_text(np_artist,playing_something?current.artist.c_str():"");
    lv_obj_set_flag(np_full,LV_OBJ_FLAG_HIDDEN,!playing_something);
    lv_obj_set_flag(np_empty,LV_OBJ_FLAG_HIDDEN,playing_something);
    lv_label_set_text(np_empty_title,selected.id.empty()?"No room selected":"Nothing playing");
    lv_label_set_text(lv_obj_get_child(np_empty_button,0),selected.id.empty()?"Choose a room":"Browse favorites");
    auto supports=[](const char* a){ return (","+current.actions+",").find(std::string(",")+a+",")!=std::string::npos; };
    set_icon(np_play,current.playback=="PLAYING"?LV_SYMBOL_PAUSE:LV_SYMBOL_PLAY);
    lv_obj_set_state(np_next,LV_STATE_DISABLED,!supports("Next"));
    lv_obj_set_state(np_prev,LV_STATE_DISABLED,!supports("Previous"));
    const bool group=grouped();
    render_volume();
    if(group) lv_label_set_text_fmt(np_volume_caption,"%s  Group volume · %u rooms",ICON_LINK,static_cast<unsigned>(group_of(selected).size()));
    lv_obj_set_flag(np_volume_caption,LV_OBJ_FLAG_HIDDEN,!group);
    render_progress();
}
void build_now_playing(lv_obj_t* v) {
    // Nothing playing: a dimmed title and one way forward, centered.
    np_empty=column(v,40); lv_obj_set_size(np_empty,CONTENT_W,LV_SIZE_CONTENT); lv_obj_center(np_empty);
    lv_obj_set_flex_align(np_empty,LV_FLEX_ALIGN_START,LV_FLEX_ALIGN_CENTER,LV_FLEX_ALIGN_CENTER);
    np_empty_title=text(np_empty,&font_title_56,ink::quiet,"Nothing playing");
    np_empty_button=pill_button(np_empty,"Browse favorites",false,empty_clicked);
    np_full=div(v); lv_obj_set_size(np_full,CONTENT_W,lv_pct(100));
    now_art.build(np_full,art_spec::now_side,art_spec::now_radius,LV_SYMBOL_AUDIO);
    lv_obj_align(now_art.frame,LV_ALIGN_LEFT_MID,PAD,0);
    const int x=PAD+480+56, width=CONTENT_W-x-PAD;
    // Spread top to bottom over the artwork's 480 px: title and artist, progress,
    // times, transport, volume. Worst case (two-line title, group caption)
    // is 436 px, so nothing has to squeeze.
    auto info=column(np_full,0); lv_obj_set_size(info,width,480); lv_obj_align(info,LV_ALIGN_LEFT_MID,x,0);
    lv_obj_set_flex_align(info,LV_FLEX_ALIGN_SPACE_BETWEEN,LV_FLEX_ALIGN_START,LV_FLEX_ALIGN_START);
    auto head=column(info,8); lv_obj_set_size(head,width,LV_SIZE_CONTENT);
    np_title=text(head,&font_title_36,ink::text,"Nothing playing");
    lv_obj_set_style_text_line_space(np_title,title_line_space(),0);
    lv_obj_set_width(np_title,width); lv_label_set_long_mode(np_title,LV_LABEL_LONG_DOT);
    lv_obj_set_height(np_title,LV_SIZE_CONTENT);  // clamped to two lines in render_now_playing
    // The top padding sits inside one_line's fixed height, so add it back or descenders clip.
    np_artist=one_line(text(head,&font_body_36,ink::muted),width); lv_obj_set_style_pad_top(np_artist,8,0);
    lv_obj_set_height(np_artist,lv_font_get_line_height(&font_body_36)+8);
    np_progress=lv_bar_create(info); lv_obj_set_size(np_progress,width,8);
    lv_obj_set_style_bg_color(np_progress,c(ink::raised),0); lv_obj_set_style_bg_opa(np_progress,LV_OPA_COVER,0);
    lv_obj_set_style_bg_color(np_progress,c(ink::text),LV_PART_INDICATOR);
    np_times=div(info); lv_obj_set_size(np_times,width,32);
    np_elapsed=text(np_times,&font_caption_22,ink::faint); lv_obj_align(np_elapsed,LV_ALIGN_LEFT_MID,0,0);
    np_remaining=text(np_times,&font_caption_22,ink::faint); lv_obj_align(np_remaining,LV_ALIGN_RIGHT_MID,0,0);
    auto transport=row(info,48); lv_obj_set_size(transport,width,128);
    lv_obj_set_flex_align(transport,LV_FLEX_ALIGN_CENTER,LV_FLEX_ALIGN_CENTER,LV_FLEX_ALIGN_CENTER);
    np_prev=icon_button(transport,LV_SYMBOL_PREV,104,false,transport_clicked,(void*)"Previous");
    np_play=icon_button(transport,LV_SYMBOL_PLAY,128,true,transport_clicked,(void*)"Toggle");
    np_next=icon_button(transport,LV_SYMBOL_NEXT,104,false,transport_clicked,(void*)"Next");
    // Volume and its group caption move as one block in the spread.
    auto volume_block=column(info,0); lv_obj_set_size(volume_block,width,LV_SIZE_CONTENT);
    auto volume=row(volume_block,24); lv_obj_set_size(volume,width,96); lv_obj_set_style_pad_top(volume,8,0);
    // A small 56 px mute button, its touch area widened to the 88 px minimum.
    constexpr int MUTE=56, VALUE_W=60;
    np_mute=icon_button(volume,LV_SYMBOL_VOLUME_MAX,MUTE,false,mute_clicked);
    lv_obj_set_style_text_font(lv_obj_get_child(np_mute,0),&font_body_26,0);
    lv_obj_set_ext_click_area(np_mute,(TARGET-MUTE)/2);
    np_volume=volume_slider(volume,width-MUTE-VALUE_W-2*24);
    np_volume_value=text(volume,&font_caption_22,ink::text,"0"); lv_obj_set_width(np_volume_value,VALUE_W);
    lv_obj_set_style_text_align(np_volume_value,LV_TEXT_ALIGN_RIGHT,0);
    np_volume_caption=text(volume_block,&font_caption_22,ink::faint,"Volume");
    lv_obj_set_style_pad_left(np_volume_caption,MUTE+24,0);
    lv_timer_create(progress_tick,1000,nullptr);
}

// ---- Favorites --------------------------------------------------------------------
void favorite_clicked(lv_event_t* e) {
    const auto index=reinterpret_cast<uintptr_t>(lv_event_get_user_data(e));
    if(index>=favorites.size()) return;
    if(selected.id.empty()) { ui_toast("Choose a room first",true); show_view(View::Rooms); return; }
    auto k=for_room("Favorite"); k->favorite=favorites[index];
    if(!submit(k)) { ui_toast("Still working on the last request. Try again.",true); return; }
    ui_toast("Starting "+favorites[index].title+" in "+selected.name);
    show_view(View::NowPlaying);
}
// Paints a tile: cover pixels when given, otherwise the placeholder.
void compose_tile(Tile& t,const Artwork* art) {
    lv_canvas_fill_bg(t.canvas,c(ink::bg),LV_OPA_COVER);
    lv_layer_t layer; lv_canvas_init_layer(t.canvas,&layer);
    const lv_area_t cover{0,0,TILE_W-1,TILE_W-1};
    lv_image_dsc_t dsc{};
    if(art && art->pixels) {
        dsc.header.magic=LV_IMAGE_HEADER_MAGIC; dsc.header.cf=LV_COLOR_FORMAT_RGB565;
        dsc.header.w=art->width; dsc.header.h=art->height; dsc.header.stride=art->stride;
        dsc.data_size=art->stride*art->height; dsc.data=art->pixels;
        lv_draw_image_dsc_t image; lv_draw_image_dsc_init(&image); image.src=&dsc;
        lv_draw_image(&layer,&image,&cover);
    } else {
        lv_draw_rect_dsc_t box; lv_draw_rect_dsc_init(&box);
        box.bg_color=c(ink::raised); box.radius=art_spec::tile_radius;
        lv_draw_rect(&layer,&box,&cover);
        lv_draw_label_dsc_t icon; lv_draw_label_dsc_init(&icon);
        icon.font=&font_body_32; icon.color=c(ink::faint); icon.align=LV_TEXT_ALIGN_CENTER;
        icon.text=t.radio?ICON_RADIO:LV_SYMBOL_AUDIO;
        const lv_area_t middle{0,TILE_W/2-20,TILE_W-1,TILE_W/2+20};
        lv_draw_label(&layer,&icon,&middle);
    }
    if(t.radio) {  // nearly everything is Apple Music, so only radio is marked
        const lv_area_t badge{TILE_W-62,10,TILE_W-11,61};
        lv_draw_rect_dsc_t dot; lv_draw_rect_dsc_init(&dot);
        dot.bg_color=c(ink::bg); dot.bg_opa=LV_OPA_80; dot.radius=LV_RADIUS_CIRCLE;
        lv_draw_rect(&layer,&dot,&badge);
        lv_draw_label_dsc_t icon; lv_draw_label_dsc_init(&icon);
        icon.font=&font_caption_22; icon.color=c(ink::text); icon.align=LV_TEXT_ALIGN_CENTER; icon.text=ICON_RADIO;
        const lv_area_t icon_area{badge.x1,badge.y1+13,badge.x2,badge.y2};
        lv_draw_label(&layer,&icon,&icon_area);
    }
    lv_draw_label_dsc_t title; lv_draw_label_dsc_init(&title);
    title.font=&font_body_26; title.color=c(ink::text); title.line_space=2; title.text=t.title.c_str();
    const lv_area_t title_area{0,TILE_TITLE_Y,TILE_W-1,TILE_H-1};
    lv_draw_label(&layer,&title,&title_area);
    lv_canvas_finish_layer(t.canvas,&layer);
}
// Asks the worker for a tile's cover. Tiles stay artless if the queue is
// full; the next scroll pass asks again, at most once every 10 s per tile.
void request_tile_art(size_t index) {
    auto& t=tiles[index];
    const int64_t now=esp_timer_get_time();
    if(t.have_art || now-t.art_asked<10*1000000LL) return;
    auto k=new Command; k->action="TileArt"; k->value=static_cast<int>(index); k->name=favorites[index].id;
    if(submit(k)) t.art_asked=now;
}
// Gives the rows around the viewport (one above, two below) a buffer and asks
// for their covers; buffers elsewhere stay until the budget needs them back.
constexpr int32_t FAV_PITCH=TILE_H+32;  // tile height + pad_row
void update_tile_cache() {
    const int32_t top=lv_obj_get_scroll_y(fav_grid);
    const int32_t lo_row=std::max<int32_t>(0,top/FAV_PITCH-1);
    const int32_t hi_row=(top+lv_obj_get_height(fav_grid))/FAV_PITCH+2;
    const size_t lo=static_cast<size_t>(lo_row)*FAV_COLUMNS;
    const size_t hi=std::min<size_t>(tiles.size(),static_cast<size_t>(hi_row+1)*FAV_COLUMNS);
    size_t buffered=0;
    for(size_t i=0;i<tiles.size();++i) {
        auto& t=tiles[i];
        if(i>=lo && i<hi) {
            if(!t.pixels) {
                if(!(t.pixels=lv_draw_buf_create(TILE_W,TILE_H,LV_COLOR_FORMAT_RGB565,LV_STRIDE_AUTO))) continue;
                lv_canvas_set_draw_buf(t.canvas,t.pixels);
                compose_tile(t,nullptr);
            }
            // Visible but artless: keep asking (10 s throttle inside) — the
            // first pass may have run while the screen was off and the
            // artwork worker drops tile jobs then.
            if(!t.have_art) request_tile_art(i);
        }
        if(t.pixels) ++buffered;
    }
    if(buffered<=TILE_BUFFER_LIMIT) return;
    // Over budget: release the buffers farthest from the window first.
    std::vector<std::pair<size_t,size_t>> far;  // (distance in tiles, index)
    for(size_t i=0;i<tiles.size();++i)
        if(tiles[i].pixels && (i<lo || i>=hi)) far.push_back({i<lo?lo-i:i-hi+1,i});
    std::sort(far.begin(),far.end(),std::greater<>());
    for(auto& [distance,i]:far) {
        if(buffered<=TILE_BUFFER_LIMIT) break;
        auto& t=tiles[i];
        lv_image_set_src(t.canvas,nullptr);  // detach the pixels before freeing them
        lv_draw_buf_destroy(t.pixels); t.pixels=nullptr; t.have_art=false;
        --buffered;
    }
}
void fav_scrolled(lv_event_t*) { update_tile_cache(); }
void favorites_refresh_clicked(lv_event_t*) { auto k=new Command; k->action="Refresh"; send(k); }
void render_favorites() {
    lv_obj_clean(fav_grid); tiles.clear();  // widgets first, then the pixels they showed
    lv_obj_scroll_to_y(fav_grid,0,LV_ANIM_OFF);
    if(favorites.empty()) {
        lv_label_set_text(fav_hint,"");
        auto l=text(fav_grid,&font_body_26,ink::muted,"No Apple Music or Sonos Radio favorites yet.\nAdd some in the Sonos app; they appear here automatically.");
        lv_obj_set_width(l,CONTENT_W-2*PAD);
        return;
    }
    lv_label_set_text_fmt(fav_hint,"%u favorites",static_cast<unsigned>(favorites.size()));
    // One label, reused, does the wrapping and "..." truncation for every title.
    auto measure=text(fav_grid,&font_body_26,ink::text,"");
    lv_obj_set_size(measure,TILE_W,72); lv_label_set_long_mode(measure,LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_line_space(measure,2,0); lv_obj_add_flag(measure,LV_OBJ_FLAG_IGNORE_LAYOUT);
    tiles.clear(); tiles.resize(favorites.size());
    for(size_t i=0;i<favorites.size();++i) {
        const auto& f=favorites[i];
        auto& t=tiles[i];
        t.radio=f.radio;
        lv_label_set_text(measure,f.title.c_str()); lv_obj_update_layout(measure);
        t.title=lv_label_get_text(measure);
        t.canvas=lv_canvas_create(fav_grid);  // no draw buffer until it nears the viewport
        lv_obj_set_size(t.canvas,TILE_W,TILE_H);
        lv_obj_add_flag(t.canvas,LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(t.canvas,LV_OBJ_FLAG_SCROLL_ON_FOCUS);
        lv_obj_add_event_cb(t.canvas,favorite_clicked,LV_EVENT_CLICKED,reinterpret_cast<void*>(i));
        lv_obj_set_style_outline_color(t.canvas,c(ink::accent),LV_STATE_PRESSED);
        lv_obj_set_style_outline_width(t.canvas,3,LV_STATE_PRESSED);
        lv_obj_set_style_outline_pad(t.canvas,6,LV_STATE_PRESSED);
        lv_obj_set_style_radius(t.canvas,art_spec::tile_radius,LV_STATE_PRESSED);
    }
    lv_obj_delete(measure);
    update_tile_cache();
}
void build_favorites(lv_obj_t* v) {
    lv_obj_remove_flag(v,LV_OBJ_FLAG_SCROLLABLE);  // the grid scrolls, the header stays put
    lv_obj_set_flex_flow(v,LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(v,PAD,0); lv_obj_set_style_pad_top(v,24,0); lv_obj_set_style_pad_row(v,16,0);
    auto top=div(v); lv_obj_set_size(top,CONTENT_W-2*PAD,TARGET);
    fav_hint=text(top,&font_body_26,ink::muted,"Connect to Wi-Fi in Settings to find your Sonos system.");
    lv_obj_align(fav_hint,LV_ALIGN_LEFT_MID,0,0);
    auto refresh=pill_button(top,"Refresh",false,favorites_refresh_clicked);
    lv_obj_align(refresh,LV_ALIGN_RIGHT_MID,0,0);
    fav_grid=div(v); lv_obj_add_flag(fav_grid,LV_OBJ_FLAG_SCROLLABLE);  // div() removes it
    lv_obj_set_width(fav_grid,CONTENT_W-2*PAD); lv_obj_set_flex_grow(fav_grid,1);
    lv_obj_set_flex_flow(fav_grid,LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(fav_grid,FAV_GAP,0); lv_obj_set_style_pad_row(fav_grid,32,0);
    lv_obj_set_scroll_dir(fav_grid,LV_DIR_VER); lv_obj_set_scrollbar_mode(fav_grid,LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_add_event_cb(fav_grid,fav_scrolled,LV_EVENT_SCROLL,nullptr);
    lv_obj_add_event_cb(fav_grid,fav_scrolled,LV_EVENT_SIZE_CHANGED,nullptr);
}

// ---- Queue ----------------------------------------------------------------------------
void queue_clicked(lv_event_t* e) {
    if(selected.id.empty()) return;
    auto k=for_room("QueueTrack"); k->value=static_cast<int>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(e))); send(k);
}
void build_queue(lv_obj_t* v) {
    lv_obj_set_flex_flow(v,LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(v,PAD,0); lv_obj_set_style_pad_top(v,24,0); lv_obj_set_style_pad_row(v,8,0);
    queue_header=text(v,&font_body_32,ink::text,"Up next");
    lv_obj_set_style_pad_bottom(queue_header,12,0);
    queue_list=column(v,8); lv_obj_set_width(queue_list,CONTENT_W-2*PAD); lv_obj_set_height(queue_list,LV_SIZE_CONTENT);
}

// ---- Rooms ------------------------------------------------------------------------------
// The highlighted cards are the selected room's Sonos group. Tapping another
// room joins it to that music; tapping a highlighted one takes it out (it
// stops). One room always stays selected, so there is something to control.
constexpr int ROOM_COLUMNS=2, ROOM_GAP=24, ROOM_CARD_W=(CONTENT_W-2*PAD-(ROOM_COLUMNS-1)*ROOM_GAP)/ROOM_COLUMNS, ROOM_CARD_H=204;
constexpr int64_t PENDING_US=10*1000000LL;
bool in_group(const sonos::Room& r) { return !selected.coordinator.empty() && r.coordinator==selected.coordinator; }
bool shown_selected(const sonos::Room& r) {
    if(auto p=pending.find(r.id); p!=pending.end()) return p->second.join;
    return in_group(r);
}
void render_rooms() {
    if(!room_grid || room_cards.size()!=rooms.size()) return;
    const int64_t now=esp_timer_get_time();
    for(auto p=pending.begin();p!=pending.end();) p=now-p->second.at>PENDING_US?pending.erase(p):std::next(p);
    for(size_t i=0;i<rooms.size();++i) {
        const auto& r=rooms[i]; auto& rc=room_cards[i];
        const bool on=shown_selected(r);
        lv_obj_set_style_border_color(rc.card,c(on?ink::accent:ink::surface),0);
        auto it=summaries.find(r.coordinator);
        const bool playing=it!=summaries.end() && it->second.playback=="PLAYING";
        const std::string line=it==summaries.end()?"":it->second.title.empty()?"Nothing playing":it->second.title+(it->second.artist.empty()?"":" · "+it->second.artist);
        lv_label_set_text(rc.detail,line.c_str());
        lv_obj_set_style_text_color(rc.detail,c(playing?ink::muted:ink::faint),0);
        lv_obj_set_flag(rc.badge,LV_OBJ_FLAG_HIDDEN,!playing);
        const bool level_known=levels.count(r.id)>0;
        if(on) {  // track line under the name, the room's own volume along the bottom
            lv_obj_align(rc.detail,LV_ALIGN_TOP_LEFT,0,52);
            lv_obj_add_flag(rc.mates,LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_flag(rc.slider,LV_OBJ_FLAG_HIDDEN,!level_known);
        } else {
            lv_obj_align(rc.detail,LV_ALIGN_BOTTOM_LEFT,0,0);
            lv_obj_add_flag(rc.slider,LV_OBJ_FLAG_HIDDEN);
            size_t mates=0; for(auto& o:rooms) if(o.coordinator==r.coordinator && o.id!=r.id) ++mates;
            if(mates) lv_label_set_text_fmt(rc.mates,"%s  with %u more",ICON_LINK,static_cast<unsigned>(mates));
            lv_obj_set_flag(rc.mates,LV_OBJ_FLAG_HIDDEN,mates==0);
        }
    }
    render_card_sliders();
}
void room_clicked(lv_event_t* e) {
    const auto index=reinterpret_cast<uintptr_t>(lv_event_get_user_data(e));
    if(index>=rooms.size()) return;
    const auto room=rooms[index];
    if(selected.id.empty()) { ui_select(room.id); submit(for_room("Poll")); return; }
    size_t chosen=0; for(auto& r:rooms) if(shown_selected(r)) ++chosen;
    const bool on=shown_selected(room);
    if(on && chosen<=1) { ui_toast("One room stays selected. Tap another room first."); return; }
    auto k=new Command; k->action=on?"Leave":"Join"; k->room=selected; k->target=room;
    if(!submit(k)) { ui_toast("Still working on the last request. Try again.",true); return; }
    pending[room.id]={!on,esp_timer_get_time()};
    if(on && room.id==selected.id)  // control moves to a room that stays
        for(auto& r:rooms) if(r.id!=room.id && shown_selected(r)) { ui_select(r.id); break; }
    render_rooms();
}
void build_room_cards() {
    lv_obj_clean(room_grid); room_cards.clear();
    for(size_t i=0;i<rooms.size();++i) {
        RoomCard rc;
        rc.card=tappable(room_grid,ROOM_CARD_W,ROOM_CARD_H,ink::surface,ink::pressed,20,room_clicked,reinterpret_cast<void*>(i));
        lv_obj_set_style_pad_all(rc.card,21,0);  // + the 3 px border every card has, so text lines up
        lv_obj_set_style_border_width(rc.card,3,0);
        rc.name=one_line(text(rc.card,&font_body_32,ink::text,rooms[i].name.c_str()),ROOM_CARD_W-48-56);
        lv_obj_align(rc.name,LV_ALIGN_TOP_LEFT,0,0);
        rc.detail=one_line(text(rc.card,&font_caption_22,ink::faint),ROOM_CARD_W-48);
        rc.mates=text(rc.card,&font_caption_22,ink::muted); lv_obj_align(rc.mates,LV_ALIGN_LEFT_MID,0,6);
        rc.badge=text(rc.card,&font_body_32,ink::accent,LV_SYMBOL_VOLUME_MAX); lv_obj_align(rc.badge,LV_ALIGN_TOP_RIGHT,0,0);
        rc.slider=volume_slider(rc.card,ROOM_CARD_W-48,card_volume_event,reinterpret_cast<void*>(i));
        lv_obj_align(rc.slider,LV_ALIGN_BOTTOM_LEFT,0,0);
        lv_obj_add_flag(rc.slider,LV_OBJ_FLAG_HIDDEN);
        room_cards.push_back(rc);
    }
    render_rooms();
}
void build_rooms(lv_obj_t* v) {
    lv_obj_set_style_pad_all(v,PAD,0); lv_obj_set_style_pad_top(v,24,0);
    room_grid=row(v,ROOM_GAP); lv_obj_set_width(room_grid,CONTENT_W-2*PAD); lv_obj_set_height(room_grid,LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(room_grid,LV_FLEX_FLOW_ROW_WRAP); lv_obj_set_style_pad_row(room_grid,ROOM_GAP,0);
}

// ---- Settings -----------------------------------------------------------------------
// Changing Wi-Fi needs the passcode set at build time (menuconfig, "Tab5 screen").
constexpr std::string_view PASSCODE=CONFIG_TAB5_SETTINGS_PASSCODE;
static_assert(PASSCODE.size()==4 && PASSCODE.find_first_not_of("0123456789")==std::string_view::npos,
              "CONFIG_TAB5_SETTINGS_PASSCODE must be exactly four digits");
void input_focused(lv_event_t* e) {
    auto target=lv_event_get_target_obj(e);
    lv_keyboard_set_textarea(keyboard,target);
    lv_obj_remove_flag(keyboard,LV_OBJ_FLAG_HIDDEN); lv_obj_move_foreground(keyboard);
}
lv_obj_t* labeled_input(lv_obj_t* parent,const char* label,const char* placeholder,int width,bool password=false) {
    auto l=text(parent,&font_caption_22,ink::muted,label);
    if(lv_obj_get_index(l)>0) lv_obj_set_style_margin_top(l,14,0);
    auto t=lv_textarea_create(parent); lv_textarea_set_one_line(t,true); lv_textarea_set_password_mode(t,password);
    lv_textarea_set_placeholder_text(t,placeholder); lv_obj_set_width(t,width);
    lv_obj_set_style_text_font(t,&font_body_32,0); lv_obj_set_style_pad_ver(t,16,0);
    lv_obj_add_event_cb(t,input_focused,LV_EVENT_FOCUSED,nullptr);
    return t;
}
void render_pin() {
    for(int i=0;i<4;++i) lv_obj_set_style_bg_color(pin_dots[i],c(i<static_cast<int>(pin_entry.size())?ink::accent:ink::raised),0);
}
void show_settings(SettingsMode mode) {
    lv_obj_add_flag(keyboard,LV_OBJ_FLAG_HIDDEN);
    pin_entry.clear(); render_pin();
    if(mode==SettingsMode::Summary) {
        const auto ssid=setting("ssid");
        lv_label_set_text(ssid_value,ssid.empty()?"Not set":ssid.c_str());
    }
    if(mode==SettingsMode::Form) {  // start from what is saved, not an abandoned edit
        lv_textarea_set_text(ssid_input,setting("ssid").c_str());
        lv_textarea_set_text(password_input,setting("password").c_str());
        lv_textarea_set_text(seed_input,setting("seed").c_str());
    }
    lv_obj_set_flag(settings_summary,LV_OBJ_FLAG_HIDDEN,mode!=SettingsMode::Summary);
    lv_obj_set_flag(settings_passcode,LV_OBJ_FLAG_HIDDEN,mode!=SettingsMode::Passcode);
    lv_obj_set_flag(settings_form,LV_OBJ_FLAG_HIDDEN,mode!=SettingsMode::Form);
}
void edit_clicked(lv_event_t*) { show_settings(SettingsMode::Passcode); }
void key_clicked(lv_event_t* e) {
    const char key=static_cast<char>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(e)));
    if(key=='<') { if(!pin_entry.empty()) pin_entry.pop_back(); }
    else if(pin_entry.size()<4) pin_entry+=key;
    render_pin();
}
void enter_clicked(lv_event_t*) {
    if(pin_entry==PASSCODE) { show_settings(SettingsMode::Form); return; }
    ui_toast(pin_entry.size()<4?"Enter all four digits":"Wrong passcode",true);
    pin_entry.clear(); render_pin();
}
void connect_clicked(lv_event_t*) {
    auto k=new Command; k->action="Connect";
    k->ssid=lv_textarea_get_text(ssid_input); k->password=lv_textarea_get_text(password_input); k->seed=lv_textarea_get_text(seed_input);
    lv_obj_add_flag(keyboard,LV_OBJ_FLAG_HIDDEN);
    const auto ssid=k->ssid;  // submit() takes ownership; the command may already be gone
    if(!submit(k)) { ui_toast("Still working on the last request. Try again.",true); return; }
    ui_toast("Connecting to "+ssid+"...");
    // The worker saves the settings; show what was entered until it has.
    show_settings(SettingsMode::Summary); lv_label_set_text(ssid_value,ssid.c_str());
}
void render_device_info() {
    std::string info;
    if(battery_known) {
        char b[96];
        if(!battery.present) snprintf(b,sizeof b,"Battery  not detected\n");
        else if(battery.charging) snprintf(b,sizeof b,"Battery  %d%%  (charging, %.1f A)\n",battery.percent,battery.current_ma/1000.0);
        else if(battery.external) snprintf(b,sizeof b,"Battery  %d%%  (on USB power)\n",battery.percent);
        else snprintf(b,sizeof b,"Battery  %d%%  (%.2f V, %d mA)\n",battery.percent,battery.pack_mv/1000.0,-battery.current_ma);
        info+=b;
    }
    esp_netif_ip_info_t ip{}; char address[16]="-";
    if(auto netif=esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"); netif && esp_netif_get_ip_info(netif,&ip)==ESP_OK && ip.ip.addr)
        esp_ip4addr_ntoa(&ip.ip,address,sizeof address);
    info+=std::string("Address  ")+address+"\n";
    info+=std::string("Firmware  ")+esp_app_get_description()->version+"\n";
    if(auto running=esp_ota_get_running_partition()) info+=std::string("Slot  ")+running->label+"\n";
    info+=std::string("Wi-Fi  ")+(online?"connected":"disconnected");
    lv_label_set_text(device_info,info.c_str());
}
// The passcode and form panels share one 375 px slot above a full-width button.
constexpr int SETTINGS_W=600, PANEL_H=375;
void build_settings(lv_obj_t* v) {
    lv_obj_set_style_pad_all(v,PAD,0); lv_obj_set_style_pad_top(v,24,0);
    // Summary: the saved network, read-only.
    settings_summary=column(v,12); lv_obj_set_size(settings_summary,SETTINGS_W,LV_SIZE_CONTENT);
    text(settings_summary,&font_body_32,ink::text,"Wi-Fi");
    text(settings_summary,&font_caption_22,ink::muted,"Network name (2.4 GHz)");
    ssid_value=one_line(text(settings_summary,&font_body_32,ink::text),SETTINGS_W);
    auto edit=pill_button(settings_summary,"Edit",false,edit_clicked); lv_obj_set_style_margin_top(edit,20,0);
    // Passcode: four dots beside the title, a phone-style keypad below.
    settings_passcode=column(v,12); lv_obj_set_size(settings_passcode,SETTINGS_W,LV_SIZE_CONTENT);
    auto title=div(settings_passcode); lv_obj_set_size(title,SETTINGS_W,40);
    lv_obj_align(text(title,&font_body_32,ink::text,"Enter passcode"),LV_ALIGN_LEFT_MID,0,0);
    auto dots=row(title,16); lv_obj_set_size(dots,LV_SIZE_CONTENT,40); lv_obj_align(dots,LV_ALIGN_RIGHT_MID,-24,0);
    for(auto& d:pin_dots) {
        d=div(dots); lv_obj_set_size(d,20,20); lv_obj_set_style_radius(d,LV_RADIUS_CIRCLE,0);
        lv_obj_set_style_bg_opa(d,LV_OPA_COVER,0);
    }
    auto pad=div(settings_passcode); lv_obj_set_size(pad,SETTINGS_W,PANEL_H);
    lv_obj_set_style_bg_opa(pad,LV_OPA_COVER,0); lv_obj_set_style_bg_color(pad,c(ink::raised),0);
    lv_obj_set_style_radius(pad,44,0); lv_obj_set_style_pad_all(pad,4,0);
    lv_obj_set_flex_flow(pad,LV_FLEX_FLOW_ROW_WRAP); lv_obj_set_style_pad_column(pad,4,0); lv_obj_set_style_pad_row(pad,4,0);
    constexpr int key_w=(SETTINGS_W-8-2*4)/3, key_h=(PANEL_H-8-3*4)/4;
    static_assert(key_h>=TARGET,"keypad keys must stay at least the minimum tap target");
    for(char key:std::string_view("123456789 0<")) {
        if(key==' ') { lv_obj_set_size(div(pad),key_w,key_h); continue; }
        auto b=tappable(pad,key_w,key_h,ink::raised,ink::pressed,40,key_clicked,reinterpret_cast<void*>(static_cast<uintptr_t>(key)));
        const char label[2]={key,0};
        lv_obj_center(text(b,&font_body_32,ink::text,key=='<'?LV_SYMBOL_BACKSPACE:label));
    }
    auto enter=pill_button(settings_passcode,"Enter",true,enter_clicked); lv_obj_set_width(enter,SETTINGS_W);
    // Form: the three fields in the same slot, then Save.
    settings_form=column(v,12); lv_obj_set_size(settings_form,SETTINGS_W,LV_SIZE_CONTENT);
    text(settings_form,&font_body_32,ink::text,"Wi-Fi");
    auto fields=column(settings_form,6); lv_obj_set_size(fields,SETTINGS_W,PANEL_H);
    ssid_input=labeled_input(fields,"Network name (2.4 GHz)","Network",SETTINGS_W); lv_textarea_set_max_length(ssid_input,32);
    password_input=labeled_input(fields,"Password","Password",SETTINGS_W,true); lv_textarea_set_max_length(password_input,63);
    seed_input=labeled_input(fields,"Speaker IP address (optional)","Found automatically",SETTINGS_W); lv_textarea_set_max_length(seed_input,15);
    lv_textarea_set_accepted_chars(seed_input,"0123456789.");
    auto save=pill_button(settings_form,"Save",true,connect_clicked); lv_obj_set_width(save,SETTINGS_W);
    auto about=column(v,16); lv_obj_set_size(about,440,LV_SIZE_CONTENT); lv_obj_align(about,LV_ALIGN_TOP_RIGHT,0,0);
    text(about,&font_body_32,ink::text,"This controller");
    device_info=text(about,&font_body_26,ink::muted,""); lv_obj_set_width(device_info,440);
    lv_obj_set_style_text_line_space(device_info,10,0);
}

// ---- chrome -------------------------------------------------------------------------------
void toast_hide(lv_timer_t* t) { lv_obj_add_flag(toast,LV_OBJ_FLAG_HIDDEN); lv_timer_pause(t); }
void build_rail(lv_obj_t* screen) {
    auto rail=div(screen); lv_obj_set_size(rail,RAIL,H);
    lv_obj_set_style_bg_opa(rail,LV_OPA_COVER,0); lv_obj_set_style_bg_color(rail,c(ink::surface),0);
    lv_obj_set_flex_flow(rail,LV_FLEX_FLOW_COLUMN); lv_obj_set_style_pad_row(rail,4,0);
    struct Item { const char* icon; const char* label; } items[5]={
        {LV_SYMBOL_AUDIO,"Playing"},{ICON_STAR,"Favorites"},{LV_SYMBOL_LIST,"Queue"},{LV_SYMBOL_HOME,"Rooms"},{LV_SYMBOL_SETTINGS,"Settings"}};
    for(int i=0;i<5;++i) {
        if(i==4) { auto grow=div(rail); lv_obj_set_flex_grow(grow,1); }
        auto b=tappable(rail,RAIL,112,ink::surface,ink::pressed,0,nav_clicked,reinterpret_cast<void*>(static_cast<uintptr_t>(i)));
        lv_obj_set_style_bg_color(b,c(ink::accent),0); lv_obj_set_style_bg_opa(b,LV_OPA_TRANSP,0);
        lv_obj_set_flex_flow(b,LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(b,LV_FLEX_ALIGN_CENTER,LV_FLEX_ALIGN_CENTER,LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(b,10,0);
        text(b,&font_body_32,ink::muted,items[i].icon);
        text(b,&font_caption_22,ink::muted,items[i].label);
        rail_items[i]=b;
    }
}
void build_header(lv_obj_t* screen) {
    auto header=div(screen); lv_obj_set_size(header,CONTENT_W,HEADER); lv_obj_set_pos(header,RAIL,0);
    auto right=row(header,20); lv_obj_set_size(right,LV_SIZE_CONTENT,HEADER); lv_obj_align(right,LV_ALIGN_RIGHT_MID,-PAD,0);
    offline_label=text(right,&font_caption_22,ink::danger,""); lv_label_set_text_fmt(offline_label,"%s  Offline",LV_SYMBOL_WARNING);
    lv_obj_add_flag(offline_label,LV_OBJ_FLAG_HIDDEN);
    battery_label=text(right,&font_caption_22,ink::muted,"");
}
}

// ---- public API ---------------------------------------------------------------------------
void ui_build() {
    auto display=lv_display_get_default();
    lv_display_set_theme(display,lv_theme_default_init(display,c(ink::accent),c(ink::muted),true,&font_body_26));
    auto screen=lv_screen_active();
    lv_obj_set_style_bg_color(screen,c(ink::bg),0);
    lv_obj_set_style_text_color(screen,c(ink::text),0);
    lv_obj_set_style_text_font(screen,&font_body_26,0);
    lv_obj_remove_flag(screen,LV_OBJ_FLAG_SCROLLABLE);
    build_rail(screen);
    build_header(screen);
    void (*builders[5])(lv_obj_t*)={build_now_playing,build_favorites,build_queue,build_rooms,build_settings};
    for(int i=0;i<5;++i) {
        auto v=lv_obj_create(screen); lv_obj_remove_style_all(v);
        lv_obj_set_size(v,CONTENT_W,CONTENT_H); lv_obj_set_pos(v,RAIL,HEADER);
        lv_obj_set_scroll_dir(v,LV_DIR_VER); lv_obj_set_scrollbar_mode(v,LV_SCROLLBAR_MODE_ACTIVE);
        lv_obj_set_style_pad_bottom(v,48,0);
        views[i]=v;
        builders[i](v);
    }
    lv_obj_remove_flag(views[0],LV_OBJ_FLAG_SCROLLABLE);
    keyboard=lv_keyboard_create(lv_layer_top()); lv_obj_set_size(keyboard,W,320); lv_obj_align(keyboard,LV_ALIGN_BOTTOM_MID,0,0);
    lv_obj_set_style_text_font(keyboard,&font_body_32,0);
    lv_obj_add_event_cb(keyboard,[](lv_event_t*){ lv_obj_add_flag(keyboard,LV_OBJ_FLAG_HIDDEN); },LV_EVENT_READY,nullptr);
    lv_obj_add_event_cb(keyboard,[](lv_event_t*){ lv_obj_add_flag(keyboard,LV_OBJ_FLAG_HIDDEN); },LV_EVENT_CANCEL,nullptr);
    lv_obj_add_flag(keyboard,LV_OBJ_FLAG_HIDDEN);
    toast=div(lv_layer_top());
    lv_obj_set_style_bg_opa(toast,LV_OPA_COVER,0); lv_obj_set_style_bg_color(toast,c(ink::raised),0);
    lv_obj_set_style_radius(toast,LV_RADIUS_CIRCLE,0); lv_obj_set_style_pad_hor(toast,36,0); lv_obj_set_style_pad_ver(toast,20,0);
    lv_obj_set_style_shadow_width(toast,40,0); lv_obj_set_style_shadow_opa(toast,LV_OPA_50,0);
    lv_obj_set_size(toast,LV_SIZE_CONTENT,LV_SIZE_CONTENT); lv_obj_set_style_max_width(toast,760,0);
    auto toast_text=text(toast,&font_body_26,ink::text,""); lv_label_set_long_mode(toast_text,LV_LABEL_LONG_DOT);
    lv_obj_set_style_max_width(toast_text,688,0);
    // In the header's empty middle, clear of controls; 760 px centered on the
    // content area stops short of the battery status on the right.
    lv_obj_align(toast,LV_ALIGN_TOP_MID,RAIL/2,(HEADER-72)/2);  // 72: 20 + 32 px line + 20
    lv_obj_add_flag(toast,LV_OBJ_FLAG_HIDDEN);
    toast_timer=lv_timer_create(toast_hide,3500,nullptr); lv_timer_pause(toast_timer);
    render_now_playing();
    show_view(setting("ssid").empty()?View::Settings:View::NowPlaying);
}
void ui_show(View next) { DisplayLock lock; show_view(next); }
View ui_view() { DisplayLock lock; return view; }
sonos::Room ui_selected() { DisplayLock lock; return selected; }
void ui_select(const std::string& id) {
    // Called from LVGL callbacks (lock already held) and the worker; bsp's lock is recursive.
    DisplayLock lock;
    auto it=std::find_if(rooms.begin(),rooms.end(),[&](const sonos::Room& r){ return r.id==id; });
    if(it==rooms.end() || it->id==selected.id) return;
    selected=*it; have_state=false; current={};
    now_art.set(Artwork{});
    render_rooms(); render_now_playing();
}
void ui_rooms(const std::vector<sonos::Room>& fresh) {
    DisplayLock lock;
    rooms=fresh;
    auto it=std::find_if(rooms.begin(),rooms.end(),[&](const sonos::Room& r){ return r.id==selected.id; });
    if(it!=rooms.end()) selected=*it;
    else if(!rooms.empty()) {  // first load, or the selected room is gone: the startup room, else the first
        auto preferred=std::find_if(rooms.begin(),rooms.end(),[](const sonos::Room& r){ return r.name==CONFIG_TAB5_DEFAULT_ROOM; });
        selected=preferred!=rooms.end()?*preferred:rooms.front(); have_state=false; current={};
    }
    else selected={};
    // A Join/Leave the topology now shows is done.
    for(auto p=pending.begin();p!=pending.end();) {
        auto r=std::find_if(rooms.begin(),rooms.end(),[&](const sonos::Room& x){ return x.id==p->first; });
        p=(r==rooms.end() || in_group(*r)==p->second.join)?pending.erase(p):std::next(p);
    }
    // Rooms come sorted by name, so only a new or missing room means new cards.
    bool same=room_cards.size()==rooms.size();
    for(size_t i=0;same && i<rooms.size();++i) same=lv_label_get_text(room_cards[i].name)==rooms[i].name;
    if(same) render_rooms(); else build_room_cards();
    render_now_playing();
}
void ui_favorites(const std::vector<sonos::Favorite>& fresh) { DisplayLock lock; favorites=fresh; render_favorites(); }
// id+index together identify the tile, so a cover fetched for an older
// catalog can never land on a different favorite after a reload.
void ui_favorite_art(const std::string& id,size_t index,Artwork art) {
    DisplayLock lock;
    if(index>=favorites.size() || index>=tiles.size() || favorites[index].id!=id) return;
    auto& t=tiles[index];
    if(!t.pixels) return;  // released by the budget meanwhile; the fetch is dropped
    t.have_art=art.pixels!=nullptr;
    compose_tile(t,&art);  // the tile now holds these pixels; the decode is freed on return
    lv_obj_invalidate(t.canvas);
}
void ui_state(const sonos::Room& room,const sonos::State& state) {
    DisplayLock lock;
    if(room.id!=selected.id) return;
    current=state; have_state=true; position_at=esp_timer_get_time();
    levels[room.id]={state.volume,state.muted};
    if(view==View::Rooms) render_rooms();
    render_now_playing();
}
void ui_artwork(const sonos::Room& room,Artwork art) { DisplayLock lock; if(room.id==selected.id) now_art.set(std::move(art)); }
void ui_queue(const sonos::Room& room,const std::vector<sonos::QueueItem>& items,int total,int track) {
    DisplayLock lock;
    if(room.id!=selected.id) return;
    lv_obj_clean(queue_list);
    if(!total) { lv_label_set_text(queue_header,"The queue is empty"); text(queue_list,&font_body_26,ink::muted,"Radio and streams play without a queue. Start an album or playlist from Favorites."); return; }
    lv_label_set_text_fmt(queue_header,"Up next · %d tracks",total);
    for(const auto& item:items) {
        const bool now=item.number==track;
        auto r=tappable(queue_list,CONTENT_W-2*PAD,96,now?ink::surface:ink::bg,ink::pressed,16,queue_clicked,reinterpret_cast<void*>(static_cast<uintptr_t>(item.number)));
        if(!now) { lv_obj_set_style_bg_opa(r,LV_OPA_TRANSP,0); lv_obj_set_style_bg_opa(r,LV_OPA_COVER,LV_STATE_PRESSED); }
        lv_obj_set_style_pad_hor(r,24,0);
        auto n=text(r,&font_caption_22,now?ink::accent:ink::faint,now?LV_SYMBOL_PLAY:std::to_string(item.number).c_str());
        lv_obj_set_width(n,64); lv_obj_align(n,LV_ALIGN_LEFT_MID,0,0);
        auto t=one_line(text(r,&font_body_26,now?ink::accent:ink::text,item.title.c_str()),CONTENT_W-2*PAD-48-88);
        lv_obj_align(t,LV_ALIGN_TOP_LEFT,88,14);
        auto a=one_line(text(r,&font_caption_22,ink::muted,item.artist.c_str()),CONTENT_W-2*PAD-48-88);
        lv_obj_align(a,LV_ALIGN_BOTTOM_LEFT,88,-14);
    }
}
void ui_summaries(const std::vector<std::pair<std::string,sonos::Summary>>& by_coordinator) {
    DisplayLock lock;
    summaries.clear();
    for(auto& [id,s]:by_coordinator) summaries[id]=s;
    if(view==View::Rooms) render_rooms();
}
void ui_levels(const std::vector<std::pair<std::string,sonos::Level>>& by_room) {
    DisplayLock lock;
    for(auto& [id,l]:by_room) levels[id]=l;
    if(view==View::Rooms) render_rooms();
}
void ui_battery(const Battery& b) {
    DisplayLock lock;
    battery=b; battery_known=true;
    if(view==View::Settings) render_device_info();
    if(!b.present) { lv_label_set_text_fmt(battery_label,"%s  Powered",LV_SYMBOL_USB); return; }
    const bool plugged=b.charging || b.external;
    const char* symbol=b.percent>85?LV_SYMBOL_BATTERY_FULL:b.percent>60?LV_SYMBOL_BATTERY_3:
                       b.percent>35?LV_SYMBOL_BATTERY_2:b.percent>10?LV_SYMBOL_BATTERY_1:LV_SYMBOL_BATTERY_EMPTY;
    lv_label_set_text_fmt(battery_label,"%s%s  %d%%",plugged?LV_SYMBOL_CHARGE "  ":"",symbol,b.percent);
    lv_obj_set_style_text_color(battery_label,c(b.percent<=10 && !plugged?ink::danger:ink::muted),0);
    // Warn once at 15% and once at 5% on battery; plugging in re-arms both.
    static int warned_at=101;
    if(plugged) warned_at=101;
    else if(b.percent<=5 && warned_at>5) { warned_at=5; ui_toast("Battery at 5%. Plug in the controller.",true); }
    else if(b.percent<=15 && warned_at>15) { warned_at=15; ui_toast("Battery low (15%)"); }
}
void ui_online(bool now_online) { DisplayLock lock; online=now_online; lv_obj_set_flag(offline_label,LV_OBJ_FLAG_HIDDEN,online); }
void ui_toast(const std::string& message,bool error) {
    DisplayLock lock;
    lv_label_set_text(lv_obj_get_child(toast,0),message.c_str());
    lv_obj_set_style_bg_color(toast,c(error?0x4A2327:ink::raised),0);
    lv_obj_remove_flag(toast,LV_OBJ_FLAG_HIDDEN); lv_obj_move_foreground(toast);
    lv_timer_reset(toast_timer); lv_timer_resume(toast_timer);
}
