#include "engine/retained_ui_runtime.hpp"
#include "engine/text_layout.hpp"

#include <RmlUi/Core.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <RmlUi/Core/TextInputContext.h>
#include <RmlUi/Core/TextInputHandler.h>
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_stdinc.h>
#include "FontFaceHandleDefault.h"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <iomanip>
#include <mutex>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace noemancer {
namespace {

using Json = nlohmann::json;

std::mutex rml_lifetime_mutex;
bool rml_runtime_claimed = false;

std::filesystem::path default_ui_font_path() {
    if(const auto* configured=std::getenv("NOEMANCER_UI_FONT");configured!=nullptr&&*configured!='\0') {
        const std::filesystem::path path{configured};
        if(std::filesystem::is_regular_file(path))return path;
    }
    if(const char* raw_base=SDL_GetBasePath();raw_base!=nullptr) {
        const std::filesystem::path base{raw_base};
        const auto packaged=(base/"../content/fonts/LatoLatin-Regular.ttf").lexically_normal();
        if(std::filesystem::is_regular_file(packaged))return packaged;
    }
    const auto working=(std::filesystem::current_path()/"content/fonts/LatoLatin-Regular.ttf").lexically_normal();
    if(std::filesystem::is_regular_file(working))return working;
#ifdef NOEMANCER_RMLUI_DEFAULT_FONT
    const std::filesystem::path generated{NOEMANCER_RMLUI_DEFAULT_FONT};
    if(std::filesystem::is_regular_file(generated))return generated;
#endif
    return {};
}

struct FontCandidate final {
    std::string id;
    std::filesystem::path path;
    std::vector<std::string> scripts;
};

std::vector<FontCandidate> available_font_candidates() {
    std::vector<FontCandidate> candidates;
    const auto add_first = [&](std::string id, std::vector<std::string> scripts,
                               const std::initializer_list<std::filesystem::path> paths) {
        for (const auto& path : paths) {
            if (!path.empty() && std::filesystem::is_regular_file(path)) {
                candidates.push_back({std::move(id), path, std::move(scripts)});
                return;
            }
        }
    };
#ifdef _WIN32
    const auto windows_root = std::getenv("WINDIR") ? std::filesystem::path(std::getenv("WINDIR"))
                                                     : std::filesystem::path("C:/Windows");
    const auto fonts = windows_root / "Fonts";
    add_first("font.platform.cjk", {"Han", "Hiragana", "Katakana", "Hangul"},
              {fonts / "msyh.ttc", fonts / "msyhl.ttc", fonts / "simhei.ttf"});
    add_first("font.platform.complex", {"Arabic", "Hebrew", "Latin"},
              {fonts / "segoeui.ttf", fonts / "arial.ttf"});
    add_first("font.platform.emoji", {"Emoji"}, {fonts / "seguiemj.ttf"});
#elif defined(__APPLE__)
    add_first("font.platform.cjk", {"Han", "Hiragana", "Katakana", "Hangul"},
              {"/System/Library/Fonts/PingFang.ttc", "/System/Library/Fonts/Hiragino Sans GB.ttc"});
    add_first("font.platform.complex", {"Arabic", "Hebrew", "Latin"},
              {"/System/Library/Fonts/SFArabic.ttf", "/System/Library/Fonts/Helvetica.ttc"});
    add_first("font.platform.emoji", {"Emoji"}, {"/System/Library/Fonts/Apple Color Emoji.ttc"});
#else
    add_first("font.platform.cjk", {"Han", "Hiragana", "Katakana", "Hangul"},
              {"/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
               "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc"});
    add_first("font.platform.complex", {"Arabic", "Hebrew", "Latin"},
              {"/usr/share/fonts/truetype/noto/NotoSansArabic-Regular.ttf",
               "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"});
    add_first("font.platform.emoji", {"Emoji"},
              {"/usr/share/fonts/truetype/noto/NotoColorEmoji.ttf"});
#endif
    return candidates;
}

bool covers_script(const std::vector<FontCandidate>& candidates, const std::string_view script) {
    return std::ranges::any_of(candidates, [&](const FontCandidate& candidate) {
        return std::ranges::find(candidate.scripts, script) != candidate.scripts.end();
    });
}

Rml::Input::KeyIdentifier retained_key_identifier(const RetainedUiKey key) {
    using enum Rml::Input::KeyIdentifier;
    switch (key) {
    case RetainedUiKey::backspace: return KI_BACK;
    case RetainedUiKey::tab: return KI_TAB;
    case RetainedUiKey::enter: return KI_RETURN;
    case RetainedUiKey::escape: return KI_ESCAPE;
    case RetainedUiKey::home: return KI_HOME;
    case RetainedUiKey::end: return KI_END;
    case RetainedUiKey::left: return KI_LEFT;
    case RetainedUiKey::right: return KI_RIGHT;
    case RetainedUiKey::up: return KI_UP;
    case RetainedUiKey::down: return KI_DOWN;
    case RetainedUiKey::insert_key: return KI_INSERT;
    case RetainedUiKey::delete_key: return KI_DELETE;
    case RetainedUiKey::a: return KI_A;
    case RetainedUiKey::c: return KI_C;
    case RetainedUiKey::v: return KI_V;
    case RetainedUiKey::x: return KI_X;
    case RetainedUiKey::y: return KI_Y;
    case RetainedUiKey::z: return KI_Z;
    default: return KI_UNKNOWN;
    }
}

int retained_modifiers(const std::uint32_t modifiers) {
    int result{};
    if ((modifiers & retained_ui_modifier_ctrl) != 0U) result |= Rml::Input::KM_CTRL;
    if ((modifiers & retained_ui_modifier_shift) != 0U) result |= Rml::Input::KM_SHIFT;
    if ((modifiers & retained_ui_modifier_alt) != 0U) result |= Rml::Input::KM_ALT;
    if ((modifiers & retained_ui_modifier_caps_lock) != 0U) result |= Rml::Input::KM_CAPSLOCK;
    if ((modifiers & retained_ui_modifier_num_lock) != 0U) result |= Rml::Input::KM_NUMLOCK;
    return result;
}

std::int32_t utf8_length(const std::string_view value) {
    std::int32_t result{};
    for (const unsigned char byte : value) if ((byte & 0xc0U) != 0x80U) ++result;
    return result;
}

std::string escape_markup(const std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (const char character : value) {
        switch (character) {
        case '&': escaped += "&amp;"; break;
        case '<': escaped += "&lt;"; break;
        case '>': escaped += "&gt;"; break;
        case '"': escaped += "&quot;"; break;
        case '\'': escaped += "&#39;"; break;
        default: escaped += character; break;
        }
    }
    return escaped;
}

std::string compact_number(const double value) {
    std::ostringstream output;
    output<<std::setprecision(6)<<std::defaultfloat<<value;
    return output.str();
}

std::string role_class(const std::string_view role) {
    if (role == "inspector") return "surface";
    if (role == "hud") return "surface hud-surface";
    if (role == "group") return "group";
    if (role == "property") return "property-row";
    if (role == "button") return "action-button";
    if (role == "tree" || role == "list") return "selectable-collection";
    if (role == "grid") return "selectable-collection grid-collection";
    if (role == "tree-item" || role == "treeitem") return "selectable-row tree-row";
    if (role == "list-item" || role == "listitem") return "selectable-row list-row";
    if (role == "grid-item" || role == "griditem") return "selectable-card";
    if (role == "meter" || role == "status" || role == "ability-slot") return "property-row " + std::string(role);
    return "node";
}

std::string semantic_id_for_element(Rml::Element* element) {
    for (auto* current = element; current != nullptr; current = current->GetParentNode()) {
        if (current->HasAttribute("data-semantic-id"))
            return current->GetAttribute<Rml::String>("data-semantic-id", "");
    }
    return {};
}

std::string inherited_attribute(Rml::Element* element,const char* name) {
    for(auto* current=element;current!=nullptr;current=current->GetParentNode())
        if(current->HasAttribute(name))return current->GetAttribute<Rml::String>(name,"");
    return {};
}

Rml::Element* ancestor_with_role(Rml::Element* element,const std::string_view role) {
    for(auto* current=element;current!=nullptr;current=current->GetParentNode())
        if(current->GetAttribute<Rml::String>("data-role","")==role)return current;
    return nullptr;
}

std::size_t bounded_grid_columns(const Json& source,const std::size_t fallback=4U) {
    if(!source.is_number_integer()&&!source.is_number_unsigned())
        return std::clamp(fallback,std::size_t{1U},std::size_t{12U});
    try{return static_cast<std::size_t>(std::clamp(source.get<std::int64_t>(),INT64_C(1),INT64_C(12)));}
    catch(...){return std::clamp(fallback,std::size_t{1U},std::size_t{12U});}
}

bool selectable_row_role(const std::string_view role) {
    return role=="tree-item"||role=="treeitem"||role=="list-item"||role=="listitem"||
        role=="grid-item"||role=="griditem";
}

Rml::Element* selectable_row(Rml::Element* element) {
    for(auto* current=element;current!=nullptr;current=current->GetParentNode())
        if(selectable_row_role(current->GetAttribute<Rml::String>("data-role","")))return current;
    return nullptr;
}

Rml::Element* selectable_collection(Rml::Element* element) {
    for(auto* current=element;current!=nullptr;current=current->GetParentNode()) {
        const auto role=current->GetAttribute<Rml::String>("data-role","");
        if(role=="tree"||role=="list"||role=="grid")return current;
    }
    return nullptr;
}

void collect_selectable_rows(Rml::Element* element,std::vector<Rml::Element*>& rows,const bool visible_only=true) {
    if(element==nullptr)return;
    if(selectable_row_role(element->GetAttribute<Rml::String>("data-role",""))&&
       element->GetAttribute<Rml::String>("data-enabled","true")=="true"&&(!visible_only||element->IsVisible(true)))
        rows.push_back(element);
    for(int index=0;index<element->GetNumChildren();++index)
        collect_selectable_rows(element->GetChild(index),rows,visible_only);
}

std::optional<double> numeric_control_value(Rml::Element* root,const char* selector) {
    if(root==nullptr)return std::nullopt;
    auto* element=root->QuerySelector(selector);
    auto* control=rmlui_dynamic_cast<Rml::ElementFormControl*>(element);
    if(control==nullptr)return std::nullopt;
    try {
        const auto value=std::stod(control->GetValue());
        return std::isfinite(value)?std::optional<double>{value}:std::nullopt;
    } catch(...) { return std::nullopt; }
}

class CaptureActionListener final : public Rml::EventListener {
public:
    void ProcessEvent(Rml::Event& event) override {
        auto* target=event.GetTargetElement();
        if(target==nullptr)return;
        if(event==Rml::EventId::Keydown) {
            process_selectable_key(target,event);
            return;
        }
        if(event==Rml::EventId::Click)
            if(auto* row=selectable_row(target);row!=nullptr)select(row,true);
        if(event==Rml::EventId::Click&&inherited_attribute(target,"data-local-action")=="toggle-group") {
            auto* group=ancestor_with_role(target,"group");
            if(group==nullptr)return;
            const auto expanded=group->GetAttribute<Rml::String>("data-expanded","true")!="true";
            apply_expansion(group,expanded);
            const auto* document=group->GetOwnerDocument();
            remember(expansion_state_, expansion_order_, local_state_key(inherited_attribute(group,"data-surface-id"),
                document==nullptr?std::string{}:std::string(document->GetId()),semantic_id_for_element(group)),expanded);
            return;
        }
        emit_action(target,event==Rml::EventId::Change,event);
    }

    [[nodiscard]] std::vector<RetainedUiActionEvent> consume() {
        std::vector<RetainedUiActionEvent> result;
        result.reserve(events_.size());
        while(!events_.empty()){result.push_back(std::move(events_.front()));events_.pop_front();}
        return result;
    }
    [[nodiscard]] std::size_t pending_count() const noexcept{return events_.size();}
    [[nodiscard]] std::uint64_t dropped_count() const noexcept{return dropped_events_;}
    [[nodiscard]] std::uint64_t sequence() const noexcept{return sequence_;}

    void restore_local_state(Rml::ElementDocument* document,const std::string_view surface_id,
                             const std::string_view document_id) {
        if(document!=nullptr)restore_local_state_recursive(document,surface_id,document_id);
    }

private:
    void emit_action(Rml::Element* target,const bool value_changed,Rml::Event& event) {
        const auto action_id=inherited_attribute(target,"data-action");
        const auto node_id=semantic_id_for_element(target);
        if(action_id.empty()||node_id.empty())return;

        if(!value_changed&&event!=Rml::EventId::Click)return;
        if(!value_changed&&rmlui_dynamic_cast<Rml::ElementFormControl*>(target)!=nullptr)return;

        const auto* document=target->GetOwnerDocument();
        const auto binding_source=inherited_attribute(target,"data-binding");
        const auto binding=Json::parse(binding_source,nullptr,false);
        std::string value_json{"null"};
        std::string control_value;
        if(auto* control=rmlui_dynamic_cast<Rml::ElementFormControl*>(target))control_value=control->GetValue();
        else if(auto* nested_input=target->QuerySelector("input"))
            if(auto* nested_control=rmlui_dynamic_cast<Rml::ElementFormControl*>(nested_input))control_value=nested_control->GetValue();
        const auto value_type=binding.is_object()?binding.value("valueType",std::string{}):std::string{};
        if(value_changed&&(value_type=="vector3"||value_type=="color3")) {
            auto* property=ancestor_with_role(target,"property");
            const auto x=numeric_control_value(property,".vector-x");
            const auto y=numeric_control_value(property,".vector-y");
            const auto z=numeric_control_value(property,".vector-z");
            if(!x||!y||!z)return;
            value_json=Json{{"x",*x},{"y",*y},{"z",*z}}.dump();
        } else if(value_changed&&target->GetAttribute<Rml::String>("type","")=="checkbox")
            value_json=Json(event.GetParameter<bool>("checked",target->HasAttribute("checked"))).dump();
        else if(value_changed&&(value_type=="f32"||value_type=="f64")) {
            try{value_json=Json(std::stod(control_value)).dump();}catch(...){value_json=Json(control_value).dump();}
        } else if(value_changed&&(value_type=="i32"||value_type=="u32")) {
            try{value_json=Json(std::stoll(control_value)).dump();}catch(...){value_json=Json(control_value).dump();}
        } else if(value_changed)value_json=Json(control_value).dump();
        RetainedUiActionEvent action{.sequence=++sequence_,
            .kind=value_changed?RetainedUiActionKind::value_changed:RetainedUiActionKind::invoke,
            .surface_id=inherited_attribute(target,"data-surface-id"),
            .document_id=document==nullptr?std::string{}:std::string(document->GetId()),
            .node_id=node_id,.action_id=action_id,
            .binding_json=binding.is_object()?binding.dump():Json{{"value",binding_source}}.dump(),
            .value_json=std::move(value_json)};
        if(events_.size()>=maximum_events){events_.pop_front();++dropped_events_;}
        events_.push_back(std::move(action));
    }

    void emit_invoke(Rml::Element* target) {
        const auto action_id=inherited_attribute(target,"data-action");
        const auto node_id=semantic_id_for_element(target);
        if(action_id.empty()||node_id.empty())return;
        const auto* document=target->GetOwnerDocument();
        const auto binding_source=inherited_attribute(target,"data-binding");
        const auto binding=Json::parse(binding_source,nullptr,false);
        RetainedUiActionEvent action{.sequence=++sequence_,.kind=RetainedUiActionKind::invoke,
            .surface_id=inherited_attribute(target,"data-surface-id"),
            .document_id=document==nullptr?std::string{}:std::string(document->GetId()),
            .node_id=node_id,.action_id=action_id,
            .binding_json=binding.is_object()?binding.dump():Json{{"value",binding_source}}.dump(),
            .value_json="null"};
        if(events_.size()>=maximum_events){events_.pop_front();++dropped_events_;}
        events_.push_back(std::move(action));
    }

    void select(Rml::Element* row,const bool focus) {
        auto* collection=selectable_collection(row);
        if(collection==nullptr)return;
        std::vector<Rml::Element*> rows;collect_selectable_rows(collection,rows);
        for(auto* candidate:rows) {
            const auto selected=candidate==row;
            candidate->SetAttribute("data-selected",selected?"true":"false");
            candidate->SetAttribute("aria-selected",selected?"true":"false");
            candidate->SetClass("selected",selected);
        }
        const auto* document=row->GetOwnerDocument();
        const auto key=local_state_key(inherited_attribute(row,"data-surface-id"),
            document==nullptr?std::string{}:std::string(document->GetId()),semantic_id_for_element(collection));
        remember(selection_state_,selection_order_,key,semantic_id_for_element(row));
        if(focus)static_cast<void>(row->Focus(true));
    }

    void process_selectable_key(Rml::Element* target,Rml::Event& event) {
        auto* row=selectable_row(target);auto* collection=selectable_collection(row);
        if(row==nullptr||collection==nullptr)return;
        const auto key=static_cast<Rml::Input::KeyIdentifier>(event.GetParameter<int>("key_identifier",0));
        if(key==Rml::Input::KI_RETURN) {emit_invoke(row);event.StopPropagation();return;}
        const auto collection_role=collection->GetAttribute<Rml::String>("data-role","");
        const auto grid=collection_role=="grid";
        if(key!=Rml::Input::KI_UP&&key!=Rml::Input::KI_DOWN&&key!=Rml::Input::KI_HOME&&key!=Rml::Input::KI_END&&
           (!grid||(key!=Rml::Input::KI_LEFT&&key!=Rml::Input::KI_RIGHT)))return;
        std::vector<Rml::Element*> rows;collect_selectable_rows(collection,rows);
        if(rows.empty())return;
        const auto found=std::ranges::find(rows,row);
        std::size_t index=found==rows.end()?0U:static_cast<std::size_t>(std::distance(rows.begin(),found));
        std::size_t columns=1U;
        if(grid) {
            try { columns=static_cast<std::size_t>(std::clamp(
                std::stoi(collection->GetAttribute<Rml::String>("data-grid-columns","1")),1,12)); }
            catch(...) { columns=1U; }
        }
        if(key==Rml::Input::KI_HOME)index=0U;
        else if(key==Rml::Input::KI_END)index=rows.size()-1U;
        else if(grid&&key==Rml::Input::KI_LEFT&&index>0U)--index;
        else if(grid&&key==Rml::Input::KI_RIGHT&&index+1U<rows.size())++index;
        else if(key==Rml::Input::KI_UP&&index>=(grid?columns:1U))index-=grid?columns:1U;
        else if(key==Rml::Input::KI_DOWN&&index+(grid?columns:1U)<rows.size())index+=grid?columns:1U;
        select(rows[index],true);event.StopPropagation();
    }

    template<class Value>
    static void remember(std::unordered_map<std::string,Value>& state,std::deque<std::string>& order,
                         std::string key,Value value) {
        if(!state.contains(key)) {
            if(order.size()>=maximum_local_states){state.erase(order.front());order.pop_front();}
            order.push_back(key);
        }
        state[std::move(key)]=std::move(value);
    }

    static std::string local_state_key(const std::string_view surface_id,const std::string_view document_id,
                                       const std::string_view node_id) {
        return std::string(surface_id)+"\n"+std::string(document_id)+"\n"+std::string(node_id);
    }
    static void apply_expansion(Rml::Element* group,const bool expanded) {
        group->SetAttribute("data-expanded",expanded?"true":"false");
        group->SetClass("collapsed",!expanded);
        if(auto* chevron=group->QuerySelector(".group-chevron"))chevron->SetInnerRML(expanded?"&#8722;":"+");
    }
    void restore_local_state_recursive(Rml::Element* element,const std::string_view surface_id,
                                       const std::string_view document_id) {
        if(element->GetAttribute<Rml::String>("data-role","")=="group") {
            const auto node_id=semantic_id_for_element(element);
            const auto found=expansion_state_.find(local_state_key(surface_id,document_id,node_id));
            if(found!=expansion_state_.end())apply_expansion(element,found->second);
        }
        const auto role=element->GetAttribute<Rml::String>("data-role","");
        if(role=="tree"||role=="list"||role=="grid") {
            const auto found=selection_state_.find(local_state_key(surface_id,document_id,semantic_id_for_element(element)));
            if(found!=selection_state_.end()) {
                std::vector<Rml::Element*> rows;collect_selectable_rows(element,rows,false);
                for(auto* row:rows) {
                    const auto selected=semantic_id_for_element(row)==found->second;
                    row->SetAttribute("data-selected",selected?"true":"false");
                    row->SetAttribute("aria-selected",selected?"true":"false");
                    row->SetClass("selected",selected);
                }
            }
        }
        for(int index=0;index<element->GetNumChildren();++index)
            restore_local_state_recursive(element->GetChild(index),surface_id,document_id);
    }
    static constexpr std::size_t maximum_events=128;
    static constexpr std::size_t maximum_local_states=256;
    std::deque<RetainedUiActionEvent> events_;
    std::unordered_map<std::string,bool> expansion_state_;
    std::deque<std::string> expansion_order_;
    std::unordered_map<std::string,std::string> selection_state_;
    std::deque<std::string> selection_order_;
    std::uint64_t sequence_{};
    std::uint64_t dropped_events_{};
};

void collect_element_observations(Rml::Element* element, const std::string& semantic_parent, Json& nodes,
                                  Json& overflow_nodes) {
    auto next_parent = semantic_parent;
    if (element->HasAttribute("data-semantic-id")) {
        const auto semantic_id = element->GetAttribute<Rml::String>("data-semantic-id", "");
        const auto overflow_x = element->GetScrollWidth() > element->GetClientWidth() + 0.5F;
        const auto overflow_y = element->GetScrollHeight() > element->GetClientHeight() + 0.5F;
        Json observation{
            {"id", semantic_id},
            {"parentId", semantic_parent.empty() ? Json(nullptr) : Json(semantic_parent)},
            {"role", element->GetAttribute<Rml::String>("data-role", std::string(element->GetTagName()))},
            {"implementation", {{"tag", element->GetTagName()}, {"classes", element->GetClassNames()}}},
            {"visible", element->IsVisible(true)},
            {"layout", {{"x", element->GetAbsoluteLeft()}, {"y", element->GetAbsoluteTop()},
                         {"width", element->GetOffsetWidth()}, {"height", element->GetOffsetHeight()},
                         {"scrollWidth", element->GetScrollWidth()}, {"scrollHeight", element->GetScrollHeight()},
                         {"overflowX", overflow_x}, {"overflowY", overflow_y}}}
        };
        if (element->HasAttribute("data-binding"))
            observation["binding"] = element->GetAttribute<Rml::String>("data-binding", "");
        if (element->HasAttribute("data-action"))
            observation["action"] = element->GetAttribute<Rml::String>("data-action", "");
        if(element->HasAttribute("data-grid-columns"))
            observation["collection"]["gridColumns"]=element->GetAttribute<int>("data-grid-columns",1);
        if(element->HasAttribute("data-image-source"))
            observation["presentation"]["imageSource"]=element->GetAttribute<Rml::String>("data-image-source","");
        if(element->HasAttribute("data-metadata")) {
            const auto metadata=Json::parse(element->GetAttribute<Rml::String>("data-metadata","{}"),nullptr,false);
            if(metadata.is_object())observation["metadata"]=metadata;
        }
        if(element->HasAttribute("data-expanded"))
            observation["state"]["expanded"]=element->GetAttribute<Rml::String>("data-expanded","true")=="true";
        if(element->HasAttribute("data-enabled"))
            observation["state"]["enabled"]=element->GetAttribute<Rml::String>("data-enabled","true")=="true";
        if(element->HasAttribute("data-editable"))
            observation["state"]["editable"]=element->GetAttribute<Rml::String>("data-editable","false")=="true";
        if(element->HasAttribute("data-error"))
            observation["state"]["error"]=element->GetAttribute<Rml::String>("data-error","");
        if(element->HasAttribute("data-selected"))
            observation["state"]["selected"]=element->GetAttribute<Rml::String>("data-selected","false")=="true";
        if(selectable_row_role(element->GetAttribute<Rml::String>("data-role","")))
            observation["state"]["focused"]=element->IsPseudoClassSet("focus");
        if (auto* input = element->QuerySelector("input")) {
            if (auto* control = rmlui_dynamic_cast<Rml::ElementFormControl*>(input))
                observation["editableValue"] = control->GetValue();
        } else if (auto* control = rmlui_dynamic_cast<Rml::ElementFormControl*>(element)) {
            observation["editableValue"] = control->GetValue();
        }
        nodes.push_back(std::move(observation));
        if (overflow_x || overflow_y)
            overflow_nodes.push_back({{"id", semantic_id}, {"overflowX", overflow_x}, {"overflowY", overflow_y}});
        next_parent = semantic_id;
    }
    for (int index = 0; index < element->GetNumChildren(); ++index)
        collect_element_observations(element->GetChild(index), next_parent, nodes, overflow_nodes);
}

class CaptureSystemInterface final : public Rml::SystemInterface {
public:
    double GetElapsedTime() override {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
    }

    bool LogMessage(const Rml::Log::Type type, const Rml::String& message) override {
        logs_.push_back({{"severity", static_cast<int>(type)}, {"message", message}});
        return true;
    }

    void ActivateKeyboard(const Rml::Vector2f caret_position, const float line_height) override {
        keyboard_.active = true;
        keyboard_.caret_x = static_cast<std::int32_t>(std::lround(caret_position.x));
        keyboard_.caret_y = static_cast<std::int32_t>(std::lround(caret_position.y));
        keyboard_.line_height = std::max(1, static_cast<std::int32_t>(std::lround(line_height)));
        ++keyboard_.revision;
    }

    void DeactivateKeyboard() override {
        keyboard_.active = false;
        ++keyboard_.revision;
    }

    [[nodiscard]] const Json& logs() const noexcept { return logs_; }
    [[nodiscard]] Json log_summary() const {
        Json counts = Json::object();
        Json samples = Json::array();
        std::unordered_map<std::string, bool> seen;
        for (const auto& entry : logs_) {
            const auto severity = std::to_string(entry.at("severity").get<int>());
            counts[severity] = counts.value(severity, 0U) + 1U;
            const auto message = entry.at("message").get<std::string>();
            if (samples.size() < 8 && seen.emplace(message, true).second) samples.push_back(entry);
        }
        return {{"total", logs_.size()}, {"countsBySeverity", std::move(counts)}, {"samples", std::move(samples)},
                {"truncated", logs_.size() > samples.size()}};
    }
    [[nodiscard]] RetainedUiKeyboardRequest keyboard_request() const noexcept { return keyboard_; }

private:
    std::chrono::steady_clock::time_point start_{std::chrono::steady_clock::now()};
    Json logs_{Json::array()};
    RetainedUiKeyboardRequest keyboard_{};
};

class CaptureRenderInterface final : public Rml::RenderInterface {
public:
    struct Geometry final {
        std::vector<Rml::Vertex> vertices;
        std::vector<int> indices;
    };
    struct DrawRecord final {
        Rml::CompiledGeometryHandle geometry{};
        Rml::Vector2f translation{};
        Rml::TextureHandle texture{};
        bool scissor_enabled{};
        Rml::Rectanglei scissor{};
    };
    struct Texture final {
        Rml::Vector2i dimensions{};
        std::uint64_t revision{1};
        std::shared_ptr<const std::vector<std::uint8_t>> rgba8;
    };
    struct RegisteredImage final {
        Rml::Vector2i dimensions{};
        std::uint64_t revision{};
        std::shared_ptr<const std::vector<std::uint8_t>> rgba8;
    };

    Rml::CompiledGeometryHandle CompileGeometry(
        const Rml::Span<const Rml::Vertex> vertices,
        const Rml::Span<const int> indices) override {
        const auto handle = next_geometry_++;
        geometries_.emplace(handle, Geometry{
            std::vector<Rml::Vertex>(vertices.begin(), vertices.end()),
            std::vector<int>(indices.begin(), indices.end())});
        return handle;
    }

    void RenderGeometry(const Rml::CompiledGeometryHandle geometry, const Rml::Vector2f translation,
                        const Rml::TextureHandle texture) override {
        const auto found = geometries_.find(geometry);
        if (found == geometries_.end()) return;
        draw_records_.push_back({geometry,translation,texture,scissor_enabled_,scissor_});
        draws_.push_back({{"geometry", geometry}, {"vertexCount", found->second.vertices.size()},
                          {"indexCount", found->second.indices.size()}, {"triangleCount", found->second.indices.size() / 3},
                          {"translation", {translation.x, translation.y}}, {"texture", texture},
                          {"scissorEnabled", scissor_enabled_},
                          {"scissor", {scissor_.Left(), scissor_.Top(), scissor_.Width(), scissor_.Height()}}});
    }

    void ReleaseGeometry(const Rml::CompiledGeometryHandle geometry) override { geometries_.erase(geometry); }
    Rml::TextureHandle LoadTexture(Rml::Vector2i& dimensions, const Rml::String& source) override {
        const auto found=registered_images_.find(source);
        if(found==registered_images_.end()){dimensions={0,0};return 0;}
        dimensions=found->second.dimensions;
        const auto handle=next_texture_++;
        textures_.emplace(handle,Texture{dimensions,found->second.revision,found->second.rgba8});
        texture_sources_.emplace(handle,source);
        return handle;
    }
    Rml::TextureHandle GenerateTexture(const Rml::Span<const Rml::byte> source,
                                       const Rml::Vector2i dimensions) override {
        if (dimensions.x <= 0 || dimensions.y <= 0) return 0;
        const auto expected = static_cast<std::size_t>(dimensions.x) *
                              static_cast<std::size_t>(dimensions.y) * 4U;
        if (source.size() < expected) return 0;
        const auto handle = next_texture_++;
        textures_.emplace(handle, Texture{
            dimensions,
            1,
            std::make_shared<const std::vector<std::uint8_t>>(source.begin(),source.begin()+expected)});
        return handle;
    }
    void ReleaseTexture(const Rml::TextureHandle texture) override {
        textures_.erase(texture);texture_sources_.erase(texture);
    }

    [[nodiscard]] RetainedUiImageReceipt register_image(
        const std::string_view source,const std::uint32_t width,const std::uint32_t height,
        const std::span<const std::uint8_t> rgba8) {
        if(source.empty()||source.size()>maximum_source_bytes||
           !std::ranges::all_of(source,[](const unsigned char value){return value>=0x21U&&value<=0x7eU;}))
            return {false,"ui.image-source-invalid","imageSource must be 1-1024 printable non-space ASCII bytes.",
                std::string(source),image_revision_,resident_image_bytes_};
        if(width==0U||height==0U||width>maximum_dimension||height>maximum_dimension)
            return {false,"ui.image-dimensions-invalid","RGBA8 image dimensions must be within 1-2048.",
                std::string(source),image_revision_,resident_image_bytes_};
        const auto expected=static_cast<std::uint64_t>(width)*static_cast<std::uint64_t>(height)*4ULL;
        if(expected>maximum_image_bytes||rgba8.size()!=expected)
            return {false,expected>maximum_image_bytes?"ui.image-bytes-exceeded":"ui.image-bytes-mismatch",
                expected>maximum_image_bytes?"RGBA8 image exceeds the 8 MiB per-image budget.":
                    "RGBA8 byte count must exactly equal width * height * 4.",
                std::string(source),image_revision_,resident_image_bytes_};
        const auto key=std::string(source);
        const auto existing=registered_images_.find(key);
        const auto replacing=existing!=registered_images_.end();
        if(!replacing&&registered_images_.size()>=maximum_images)
            return {false,"ui.image-count-exceeded","The retained image registry reached its 256-image budget.",
                key,image_revision_,resident_image_bytes_};
        const auto prior=replacing?existing->second.rgba8->size():0U;
        if(expected>maximum_resident_image_bytes-(resident_image_bytes_-prior))
            return {false,"ui.image-resident-bytes-exceeded","The retained image registry exceeds its 64 MiB resident budget.",
                key,image_revision_,resident_image_bytes_};
        auto pixels=std::make_shared<const std::vector<std::uint8_t>>(rgba8.begin(),rgba8.end());
        const auto revision=++image_revision_;
        registered_images_[key]={{static_cast<int>(width),static_cast<int>(height)},revision,pixels};
        resident_image_bytes_=resident_image_bytes_-prior+pixels->size();
        for(const auto& [handle,texture_source]:texture_sources_)if(texture_source==key)
            textures_[handle]={{static_cast<int>(width),static_cast<int>(height)},revision,pixels};
        return {true,replacing?"ui.image-replaced":"ui.image-registered",
            replacing?"RGBA8 image replaced deterministically.":"RGBA8 image registered.",
            key,revision,resident_image_bytes_};
    }

    [[nodiscard]] RetainedUiImageReceipt remove_image(const std::string_view source) {
        const auto found=registered_images_.find(std::string(source));
        if(found==registered_images_.end())return {false,"ui.image-not-found","imageSource is not registered.",
            std::string(source),image_revision_,resident_image_bytes_};
        resident_image_bytes_-=found->second.rgba8->size();registered_images_.erase(found);
        const auto revision=++image_revision_;
        for(const auto& [handle,texture_source]:texture_sources_)if(texture_source==source)textures_.erase(handle);
        return {true,"ui.image-removed","RGBA8 image removed.",std::string(source),revision,resident_image_bytes_};
    }
    void EnableScissorRegion(const bool enabled) override { scissor_enabled_ = enabled; }
    void SetScissorRegion(const Rml::Rectanglei region) override { scissor_ = region; }

    void begin_frame() { draws_ = Json::array(); draw_records_.clear(); }
    [[nodiscard]] Json packet() const {
        std::size_t triangles = 0;
        for (const auto& draw : draws_) triangles += draw.at("triangleCount").get<std::size_t>();
        Json samples = Json::array();
        for (std::size_t index = 0; index < std::min<std::size_t>(8, draws_.size()); ++index)
            samples.push_back(draws_.at(index));
        return {{"schemaVersion", "noemancer.ui-render-packet/0.1"}, {"backend", "capture"},
                {"drawCount", draws_.size()}, {"triangleCount", triangles},
                {"residentGeometryCount", geometries_.size()}, {"drawSamples", std::move(samples)},
                {"drawsTruncated", draws_.size() > 8}};
    }

    [[nodiscard]] RetainedUiRenderPacket packet_data() const {
        RetainedUiRenderPacket packet;
        for(const auto& draw:draw_records_) {
            const auto found=geometries_.find(draw.geometry); if(found==geometries_.end()) continue;
            const auto base_vertex=static_cast<std::uint32_t>(packet.vertices.size());
            const auto first_index=static_cast<std::uint32_t>(packet.indices.size());
            for(const auto& vertex:found->second.vertices) packet.vertices.push_back({
                {vertex.position.x+draw.translation.x,vertex.position.y+draw.translation.y},
                static_cast<std::uint32_t>(vertex.colour.red)|static_cast<std::uint32_t>(vertex.colour.green)<<8U|
                    static_cast<std::uint32_t>(vertex.colour.blue)<<16U|static_cast<std::uint32_t>(vertex.colour.alpha)<<24U,
                {vertex.tex_coord.x,vertex.tex_coord.y}});
            for(const auto index:found->second.indices) packet.indices.push_back(base_vertex+static_cast<std::uint32_t>(index));
            packet.draws.push_back({first_index,static_cast<std::uint32_t>(found->second.indices.size()),draw.scissor_enabled,
                {draw.scissor.Left(),draw.scissor.Top(),draw.scissor.Width(),draw.scissor.Height()},static_cast<std::uint64_t>(draw.texture)});
        }
        packet.textures.reserve(textures_.size());
        for (const auto& [id, texture] : textures_) {
            if(!texture.rgba8)continue;
            packet.textures.push_back({static_cast<std::uint64_t>(id),
                static_cast<std::uint32_t>(texture.dimensions.x),
                static_cast<std::uint32_t>(texture.dimensions.y), texture.revision, *texture.rgba8});
        }
        std::ranges::sort(packet.textures, {}, &RetainedUiTexture::id);
        return packet;
    }

private:
    std::unordered_map<Rml::CompiledGeometryHandle, Geometry> geometries_;
    Rml::CompiledGeometryHandle next_geometry_{1};
    Json draws_{Json::array()};
    std::vector<DrawRecord> draw_records_;
    std::unordered_map<Rml::TextureHandle, Texture> textures_;
    std::unordered_map<std::string,RegisteredImage> registered_images_;
    std::unordered_map<Rml::TextureHandle,std::string> texture_sources_;
    Rml::TextureHandle next_texture_{1};
    std::uint64_t image_revision_{};
    std::size_t resident_image_bytes_{};
    static constexpr std::size_t maximum_source_bytes=1024U;
    static constexpr std::uint32_t maximum_dimension=2048U;
    static constexpr std::uint64_t maximum_image_bytes=8ULL*1024ULL*1024ULL;
    static constexpr std::size_t maximum_images=256U;
    static constexpr std::size_t maximum_resident_image_bytes=64U*1024U*1024U;
    bool scissor_enabled_{};
    Rml::Rectanglei scissor_{};
};

class CaptureTextInputHandler final : public Rml::TextInputHandler {
public:
    void OnActivate(Rml::TextInputContext* context) override {
        active_=context; composition_begin_=composition_end_=0; ++revision_;
    }
    void OnDeactivate(Rml::TextInputContext* context) override {
        if(active_==context) { active_=nullptr; composition_begin_=composition_end_=0; ++revision_; }
    }
    void OnDestroy(Rml::TextInputContext* context) override { OnDeactivate(context); }

    bool update_composition(const std::string_view utf8,const std::int32_t cursor,const std::int32_t selection_length) {
        if(!active_) return false;
        int begin{},end{};
        if(composition_end_>composition_begin_) { begin=composition_begin_; end=composition_end_; }
        else active_->GetSelectionRange(begin,end);
        const Rml::String composition(utf8.data(),utf8.size());
        active_->SetText(composition,begin,end);
        const auto length=utf8_length(utf8);
        composition_begin_=begin; composition_end_=begin+length;
        active_->SetCompositionRange(composition_begin_,composition_end_);
        const auto caret=begin+std::clamp(cursor<0?length:cursor,0,length);
        const auto selection=std::clamp(selection_length<0?0:selection_length,0,length-(caret-begin));
        active_->SetSelectionRange(caret,caret+selection);
        composition_utf8_=std::string(utf8); ++revision_;
        return true;
    }

    bool commit(const std::string_view utf8) {
        if(!active_||composition_end_<=composition_begin_) return false;
        const Rml::String composition(utf8.data(),utf8.size());
        active_->CommitComposition(composition);
        composition_begin_=composition_end_=0; composition_utf8_.clear(); ++revision_;
        return true;
    }

    [[nodiscard]] bool active() const noexcept { return active_!=nullptr; }
    [[nodiscard]] Json observation() const {
        return {{"active",composition_end_>composition_begin_},{"text",composition_utf8_},
                {"range",{{"start",composition_begin_},{"end",composition_end_}}},{"revision",revision_}};
    }

private:
    Rml::TextInputContext* active_{};
    std::int32_t composition_begin_{};
    std::int32_t composition_end_{};
    std::string composition_utf8_;
    std::uint64_t revision_{};
};

} // namespace

struct RetainedSurfaceState final {
    std::string context_name;
    Rml::Context* context{};
    std::unordered_map<std::string,Rml::ElementDocument*> documents;
    RetainedUiRenderPacket packet;
    Json packet_summary;
};

struct RetainedUiRuntime::Impl final {
    CaptureSystemInterface system;
    CaptureRenderInterface render;
    CaptureTextInputHandler text_input;
    CaptureActionListener actions;
    Rml::Context* context{};
    std::unordered_map<std::string, Rml::ElementDocument*> documents;
    RetainedUiRenderPacket primary_packet;
    Json primary_packet_summary;
    std::unordered_map<std::string,RetainedSurfaceState> surfaces;
    std::string last_error;
    bool owns_rml{};
    bool default_font_loaded{};
    std::vector<FontCandidate> fallback_fonts;
};

RetainedUiRuntime::RetainedUiRuntime() : impl_(std::make_unique<Impl>()) {}

RetainedUiRuntime::~RetainedUiRuntime() {
    if (!impl_ || !impl_->owns_rml) return;
    if(impl_->context) {
        impl_->context->RemoveEventListener("click",&impl_->actions);
        impl_->context->RemoveEventListener("change",&impl_->actions);
        impl_->context->RemoveEventListener("keydown",&impl_->actions);
    }
    for(auto& [id,surface]:impl_->surfaces) {
        static_cast<void>(id);
        surface.context->RemoveEventListener("click",&impl_->actions);
        surface.context->RemoveEventListener("change",&impl_->actions);
        surface.context->RemoveEventListener("keydown",&impl_->actions);
    }
    impl_->documents.clear();
    if(Rml::GetTextInputHandler()==&impl_->text_input) Rml::SetTextInputHandler(nullptr);
    Rml::Shutdown();
    std::scoped_lock lock(rml_lifetime_mutex);
    rml_runtime_claimed = false;
}

bool RetainedUiRuntime::initialize(const std::uint32_t width, const std::uint32_t height, const float density_scale) {
    if (impl_->owns_rml) { impl_->last_error = "Retained UI runtime is already initialized"; return false; }
    if (width == 0 || height == 0 || !std::isfinite(density_scale) || density_scale <= 0.0F) {
        impl_->last_error = "Retained UI viewport and density scale must be positive";
        return false;
    }
    {
        std::scoped_lock lock(rml_lifetime_mutex);
        if (rml_runtime_claimed) {
            impl_->last_error = "RmlUi 6.2 has process-global state; one Noemancer owner is already active";
            return false;
        }
        rml_runtime_claimed = true;
    }
    Rml::SetSystemInterface(&impl_->system);
    Rml::SetRenderInterface(&impl_->render);
    if (!Rml::Initialise()) {
        std::scoped_lock lock(rml_lifetime_mutex);
        rml_runtime_claimed = false;
        impl_->last_error = "RmlUi 6.2 initialization failed: " + impl_->system.logs().dump();
        return false;
    }
    impl_->owns_rml = true;
    Rml::SetTextInputHandler(&impl_->text_input);
    const auto default_font=default_ui_font_path();
    if (default_font.empty() || !Rml::LoadFontFace(default_font.string())) {
        impl_->last_error = "RmlUi could not resolve or load its relocatable default font";
        return false;
    }
    impl_->default_font_loaded = true;
    for (const auto& candidate : available_font_candidates()) {
        if (Rml::LoadFontFace(candidate.path.string(), true)) impl_->fallback_fonts.push_back(candidate);
    }
    impl_->context = Rml::CreateContext("noemancer.retained-ui", {static_cast<int>(width), static_cast<int>(height)});
    if (!impl_->context) {
        impl_->last_error = "RmlUi context creation failed";
        return false;
    }
    impl_->context->SetDensityIndependentPixelRatio(density_scale);
    impl_->context->AddEventListener("click",&impl_->actions);
    impl_->context->AddEventListener("change",&impl_->actions);
    impl_->context->AddEventListener("keydown",&impl_->actions);
    impl_->last_error.clear();
    return true;
}

bool RetainedUiRuntime::load_document(const std::string_view document_id, const std::string_view rml) {
    if (!impl_->context || document_id.empty() || rml.empty()) {
        impl_->last_error = "A live context, document ID, and RML source are required";
        return false;
    }
    if (impl_->documents.contains(std::string(document_id))) {
        impl_->last_error = "Duplicate retained UI document ID: " + std::string(document_id);
        return false;
    }
    auto* document = impl_->context->LoadDocumentFromMemory(std::string(rml), "memory://ui/" + std::string(document_id) + ".rml");
    if (!document) { impl_->last_error = "RmlUi rejected document: " + std::string(document_id); return false; }
    document->SetId(std::string(document_id));
    document->SetAttribute("data-surface-id","primary");
    impl_->actions.restore_local_state(document,"primary",document_id);
    document->Show();
    impl_->documents.emplace(document_id, document);
    return update();
}

bool RetainedUiRuntime::reload_document(const std::string_view document_id, const std::string_view rml) {
    if (!impl_->context || document_id.empty() || rml.empty()) {
        impl_->last_error = "A live context, document ID, and RML source are required";
        return false;
    }
    if (const auto found=impl_->documents.find(std::string(document_id)); found!=impl_->documents.end()) {
        found->second->Close();
        impl_->documents.erase(found);
    }
    return load_document(document_id,rml);
}

bool RetainedUiRuntime::update() {
    if (!impl_->context) { impl_->last_error = "Retained UI runtime is not initialized"; return false; }
    if (!impl_->context->Update()) { impl_->last_error = "RmlUi document update failed"; return false; }
    impl_->last_error.clear();
    return true;
}

bool RetainedUiRuntime::render() {
    if (!impl_->context) { impl_->last_error = "Retained UI runtime is not initialized"; return false; }
    impl_->render.begin_frame();
    if (!impl_->context->Render()) { impl_->last_error = "RmlUi render extraction failed"; return false; }
    impl_->primary_packet=impl_->render.packet_data();
    impl_->primary_packet_summary=impl_->render.packet();
    impl_->last_error.clear();
    return true;
}

bool RetainedUiRuntime::resize(const std::uint32_t width, const std::uint32_t height, const float density_scale) {
    if (!impl_->context || width == 0 || height == 0 || !std::isfinite(density_scale) || density_scale <= 0.0F) {
        impl_->last_error = "A live context and positive viewport are required";
        return false;
    }
    impl_->context->SetDimensions({static_cast<int>(width), static_cast<int>(height)});
    impl_->context->SetDensityIndependentPixelRatio(density_scale);
    return update();
}

bool RetainedUiRuntime::pointer_move(const std::int32_t x, const std::int32_t y) {
    if (!impl_->context) return false;
    return impl_->context->ProcessMouseMove(x, y, 0);
}

bool RetainedUiRuntime::pointer_button(const std::uint32_t button, const bool pressed) {
    if (!impl_->context || button > 2) return false;
    return pressed ? impl_->context->ProcessMouseButtonDown(static_cast<int>(button), 0)
                   : impl_->context->ProcessMouseButtonUp(static_cast<int>(button), 0);
}

bool RetainedUiRuntime::pointer_leave() {
    return impl_->context ? impl_->context->ProcessMouseLeave() : false;
}

bool RetainedUiRuntime::key(const RetainedUiKey key, const bool pressed, const std::uint32_t modifiers) {
    if (!impl_->context) return false;
    const auto identifier = retained_key_identifier(key);
    if (identifier == Rml::Input::KI_UNKNOWN) return true;
    const auto propagating = pressed ? impl_->context->ProcessKeyDown(identifier, retained_modifiers(modifiers))
                                     : impl_->context->ProcessKeyUp(identifier, retained_modifiers(modifiers));
    if (pressed && key == RetainedUiKey::enter)
        return impl_->context->ProcessTextInput('\n') && propagating;
    return propagating;
}

bool RetainedUiRuntime::text_input(const std::string_view utf8) {
    if (!impl_->context || utf8.empty()) return false;
    if(impl_->text_input.commit(utf8)) return false;
    return impl_->context->ProcessTextInput(std::string(utf8));
}

bool RetainedUiRuntime::text_composition(const std::string_view utf8, const std::int32_t cursor,
                                         const std::int32_t selection_length) {
    if(!impl_->context) return false;
    return impl_->text_input.update_composition(utf8,cursor,selection_length);
}

bool RetainedUiRuntime::focus_node(const std::string_view document_id, const std::string_view semantic_node_id) {
    const auto found = impl_->documents.find(std::string(document_id));
    if (found == impl_->documents.end() || semantic_node_id.empty()) return false;
    auto* element = found->second->GetElementById(std::string(semantic_node_id));
    if (!element) return false;
    if (!selectable_row_role(element->GetAttribute<Rml::String>("data-role","")))
        if (auto* input = element->QuerySelector("input")) element = input;
    return element->Focus(true) && update();
}

RetainedUiKeyboardRequest RetainedUiRuntime::keyboard_request() const noexcept {
    return impl_ ? impl_->system.keyboard_request() : RetainedUiKeyboardRequest{};
}

std::vector<RetainedUiActionEvent> RetainedUiRuntime::consume_action_events() {
    return impl_?impl_->actions.consume():std::vector<RetainedUiActionEvent>{};
}

RetainedUiImageReceipt RetainedUiRuntime::register_image_rgba8(
    const std::string_view image_source,const std::uint32_t width,const std::uint32_t height,
    const std::span<const std::uint8_t> rgba8) {
    return impl_?impl_->render.register_image(image_source,width,height,rgba8):
        RetainedUiImageReceipt{false,"ui.runtime-unavailable","Retained UI runtime state is unavailable.",
            std::string(image_source),0U,0U};
}

RetainedUiImageReceipt RetainedUiRuntime::remove_image(const std::string_view image_source) {
    return impl_?impl_->render.remove_image(image_source):
        RetainedUiImageReceipt{false,"ui.runtime-unavailable","Retained UI runtime state is unavailable.",
            std::string(image_source),0U,0U};
}

bool RetainedUiRuntime::create_surface(const std::string_view surface_id,const std::uint32_t width,
                                       const std::uint32_t height,const float density_scale) {
    const auto valid_id=!surface_id.empty()&&surface_id!="primary"&&std::ranges::all_of(surface_id,[](const char value){
        return std::isalnum(static_cast<unsigned char>(value))||value=='.'||value=='-'||value=='_';});
    if(!impl_->owns_rml||!valid_id||width==0||height==0||!std::isfinite(density_scale)||density_scale<=0.0F||
       impl_->surfaces.contains(std::string(surface_id))) {
        impl_->last_error="A live runtime, unique safe surface ID, and positive viewport are required";return false;
    }
    const auto context_name="noemancer.retained-ui."+std::string(surface_id);
    auto* context=Rml::CreateContext(context_name,{static_cast<int>(width),static_cast<int>(height)});
    if(context==nullptr){impl_->last_error="RmlUi surface context creation failed: "+std::string(surface_id);return false;}
    context->SetDensityIndependentPixelRatio(density_scale);
    context->AddEventListener("click",&impl_->actions);
    context->AddEventListener("change",&impl_->actions);
    context->AddEventListener("keydown",&impl_->actions);
    impl_->surfaces.emplace(std::string(surface_id),RetainedSurfaceState{context_name,context,{},{},{}});
    impl_->last_error.clear();return true;
}

bool RetainedUiRuntime::destroy_surface(const std::string_view surface_id) {
    const auto found=impl_->surfaces.find(std::string(surface_id));
    if(found==impl_->surfaces.end()){impl_->last_error="Retained UI surface not found: "+std::string(surface_id);return false;}
    found->second.context->RemoveEventListener("click",&impl_->actions);
    found->second.context->RemoveEventListener("change",&impl_->actions);
    found->second.context->RemoveEventListener("keydown",&impl_->actions);
    found->second.documents.clear();
    if(!Rml::RemoveContext(found->second.context_name)){
        impl_->last_error="RmlUi surface context removal failed: "+std::string(surface_id);return false;
    }
    impl_->surfaces.erase(found);impl_->last_error.clear();return true;
}

bool RetainedUiRuntime::load_surface_document(const std::string_view surface_id,const std::string_view document_id,
                                              const std::string_view rml) {
    const auto found=impl_->surfaces.find(std::string(surface_id));
    if(found==impl_->surfaces.end()||document_id.empty()||rml.empty()){
        impl_->last_error="A live surface, document ID, and RML source are required";return false;
    }
    auto& surface=found->second;
    if(surface.documents.contains(std::string(document_id))){impl_->last_error="Duplicate retained UI surface document ID: "+std::string(document_id);return false;}
    auto* document=surface.context->LoadDocumentFromMemory(std::string(rml),"memory://ui/"+std::string(surface_id)+"/"+std::string(document_id)+".rml");
    if(document==nullptr){impl_->last_error="RmlUi rejected surface document: "+std::string(document_id);return false;}
    document->SetId(std::string(document_id));document->SetAttribute("data-surface-id",std::string(surface_id));
    impl_->actions.restore_local_state(document,surface_id,document_id);document->Show();
    surface.documents.emplace(document_id,document);return update_surface(surface_id);
}

bool RetainedUiRuntime::reload_surface_document(const std::string_view surface_id,const std::string_view document_id,
                                                const std::string_view rml) {
    const auto found=impl_->surfaces.find(std::string(surface_id));
    if(found==impl_->surfaces.end()||document_id.empty()||rml.empty()){
        impl_->last_error="A live surface, document ID, and RML source are required";return false;
    }
    if(const auto document=found->second.documents.find(std::string(document_id));document!=found->second.documents.end()){
        document->second->Close();found->second.documents.erase(document);
    }
    return load_surface_document(surface_id,document_id,rml);
}

bool RetainedUiRuntime::update_surface(const std::string_view surface_id) {
    const auto found=impl_->surfaces.find(std::string(surface_id));
    if(found==impl_->surfaces.end()){impl_->last_error="Retained UI surface not found: "+std::string(surface_id);return false;}
    if(!found->second.context->Update()){impl_->last_error="Retained UI surface update failed";return false;}
    impl_->last_error.clear();return true;
}

bool RetainedUiRuntime::render_surface(const std::string_view surface_id) {
    const auto found=impl_->surfaces.find(std::string(surface_id));
    if(found==impl_->surfaces.end()){impl_->last_error="Retained UI surface not found: "+std::string(surface_id);return false;}
    impl_->render.begin_frame();
    if(!found->second.context->Render()){impl_->last_error="Retained UI surface render extraction failed";return false;}
    found->second.packet=impl_->render.packet_data();
    found->second.packet_summary=impl_->render.packet();impl_->last_error.clear();return true;
}

bool RetainedUiRuntime::resize_surface(const std::string_view surface_id,const std::uint32_t width,
                                       const std::uint32_t height,const float density_scale) {
    const auto found=impl_->surfaces.find(std::string(surface_id));
    if(found==impl_->surfaces.end()||width==0||height==0||!std::isfinite(density_scale)||density_scale<=0.0F){
        impl_->last_error="A live surface and positive viewport are required";return false;
    }
    found->second.context->SetDimensions({static_cast<int>(width),static_cast<int>(height)});
    found->second.context->SetDensityIndependentPixelRatio(density_scale);return update_surface(surface_id);
}

bool RetainedUiRuntime::surface_pointer_move(const std::string_view surface_id,const std::int32_t x,const std::int32_t y) {
    const auto found=impl_->surfaces.find(std::string(surface_id));return found==impl_->surfaces.end()?false:found->second.context->ProcessMouseMove(x,y,0);
}
bool RetainedUiRuntime::surface_pointer_button(const std::string_view surface_id,const std::uint32_t button,const bool pressed) {
    const auto found=impl_->surfaces.find(std::string(surface_id));if(found==impl_->surfaces.end()||button>2)return false;
    return pressed?found->second.context->ProcessMouseButtonDown(static_cast<int>(button),0):
        found->second.context->ProcessMouseButtonUp(static_cast<int>(button),0);
}
bool RetainedUiRuntime::surface_pointer_leave(const std::string_view surface_id) {
    const auto found=impl_->surfaces.find(std::string(surface_id));return found==impl_->surfaces.end()?false:found->second.context->ProcessMouseLeave();
}
bool RetainedUiRuntime::surface_key(const std::string_view surface_id,const RetainedUiKey key,const bool pressed,const std::uint32_t modifiers) {
    const auto found=impl_->surfaces.find(std::string(surface_id));if(found==impl_->surfaces.end())return false;
    const auto identifier=retained_key_identifier(key);if(identifier==Rml::Input::KI_UNKNOWN)return true;
    const auto propagating=pressed?found->second.context->ProcessKeyDown(identifier,retained_modifiers(modifiers)):
        found->second.context->ProcessKeyUp(identifier,retained_modifiers(modifiers));
    if(pressed&&key==RetainedUiKey::enter)return found->second.context->ProcessTextInput('\n')&&propagating;
    return propagating;
}
bool RetainedUiRuntime::surface_text_input(const std::string_view surface_id,const std::string_view utf8) {
    const auto found=impl_->surfaces.find(std::string(surface_id));if(found==impl_->surfaces.end()||utf8.empty())return false;
    if(impl_->text_input.commit(utf8))return false;return found->second.context->ProcessTextInput(std::string(utf8));
}
bool RetainedUiRuntime::surface_text_composition(const std::string_view surface_id,const std::string_view utf8,
                                                 const std::int32_t cursor,const std::int32_t selection_length) {
    if(!impl_->surfaces.contains(std::string(surface_id)))return false;return impl_->text_input.update_composition(utf8,cursor,selection_length);
}
bool RetainedUiRuntime::focus_surface_node(const std::string_view surface_id,const std::string_view document_id,
                                           const std::string_view semantic_node_id) {
    const auto found=impl_->surfaces.find(std::string(surface_id));if(found==impl_->surfaces.end()||semantic_node_id.empty())return false;
    const auto document=found->second.documents.find(std::string(document_id));if(document==found->second.documents.end())return false;
    auto* element=document->second->GetElementById(std::string(semantic_node_id));if(element==nullptr)return false;
    if(!selectable_row_role(element->GetAttribute<Rml::String>("data-role","")))
        if(auto* input=element->QuerySelector("input"))element=input;
    return element->Focus(true)&&update_surface(surface_id);
}

bool RetainedUiRuntime::initialized() const noexcept { return impl_ && impl_->context; }
std::string_view RetainedUiRuntime::last_error() const noexcept { return impl_->last_error; }

std::string RetainedUiRuntime::observation_json(const std::string_view document_id) const {
    return surface_observation_json("primary",document_id);
}

std::string RetainedUiRuntime::surface_observation_json(const std::string_view surface_id,const std::string_view document_id) const {
    Rml::Context* context{};Rml::ElementDocument* document{};
    if(surface_id=="primary") {
        context=impl_->context;const auto found=impl_->documents.find(std::string(document_id));
        if(found!=impl_->documents.end())document=found->second;
    } else if(const auto surface=impl_->surfaces.find(std::string(surface_id));surface!=impl_->surfaces.end()) {
        context=surface->second.context;const auto found=surface->second.documents.find(std::string(document_id));
        if(found!=surface->second.documents.end())document=found->second;
    }
    if(context==nullptr||document==nullptr)
        return Json{{"schemaVersion", "noemancer.retained-ui-observation/0.1"}, {"valid", false},
                    {"code", "ui.document-not-found"}, {"surfaceId",surface_id},{"documentId", document_id}}.dump();
    const auto dimensions = context->GetDimensions();
    Json nodes = Json::array();
    Json overflow_nodes = Json::array();
    const auto shaping_stats = Rml::GetDefaultFontShapingStats();
    collect_element_observations(document, "", nodes, overflow_nodes);
    return Json{{"schemaVersion", "noemancer.retained-ui-observation/0.1"}, {"valid", true}, {"code", "ok"},
                {"implementation", { {"name", "RmlUi"}, {"version", "6.2"} }},
                {"surfaceId",surface_id},{"documentId", document_id}, {"viewport", {{"width", dimensions.x}, {"height", dimensions.y},
                    {"densityScale", context->GetDensityIndependentPixelRatio()}}},
                {"text", {{"defaultFontLoaded", impl_->default_font_loaded}, {"defaultFamily", "LatoLatin"},
                    {"fallbackFaceCount", impl_->fallback_fonts.size()},
                    {"fallbackFaces", [&] { Json faces=Json::array(); for(const auto& font:impl_->fallback_fonts)
                        faces.push_back({{"id",font.id},{"path",font.path.generic_string()},{"scripts",font.scripts}}); return faces; }()},
                    {"cjkFallbackReady", covers_script(impl_->fallback_fonts,"Han")},
                    {"complexScriptFallbackReady", covers_script(impl_->fallback_fonts,"Arabic")},
                    {"fallbackGlyphSelection", !impl_->fallback_fonts.empty()},
                    {"layoutPlan", {{"harfBuzzShaping",true},{"icuBidirectionalLayout",true},{"icuLineBreaking",true}}},
                    {"retainedGlyphRunRendering",true},
                    {"shapingStats",{{"stringsShaped",shaping_stats.strings_shaped},{"visualRuns",shaping_stats.visual_runs},
                        {"glyphsEmitted",shaping_stats.glyphs_emitted},{"fallbackRuns",shaping_stats.fallback_runs},
                        {"atlasGlyphsLoaded",shaping_stats.atlas_glyphs_loaded},{"failures",shaping_stats.failures}}},
                    {"complexTextBoundary", "HarfBuzz glyph runs feed the retained FreeType atlas and typed draw packet"}}},
                {"interaction", {{"pointerInteracting", context->IsMouseInteracting()},
                    {"hoveredNodeId", semantic_id_for_element(context->GetHoverElement())},
                    {"focusedNodeId", semantic_id_for_element(context->GetFocusElement())},
                    {"actions",{{"schemaVersion","noemancer.retained-ui-actions/0.1"},
                        {"pendingCount",impl_->actions.pending_count()},{"droppedCount",impl_->actions.dropped_count()},
                        {"lastSequence",impl_->actions.sequence()},{"boundedCapacity",128}}},
                    {"textInput", {{"active", impl_->system.keyboard_request().active},
                        {"caret", {{"x",impl_->system.keyboard_request().caret_x},{"y",impl_->system.keyboard_request().caret_y},
                                   {"lineHeight",impl_->system.keyboard_request().line_height}}},
                        {"revision",impl_->system.keyboard_request().revision},
                        {"committedUtf8Ready",true},{"compositionPreviewReady",true},
                        {"composition",impl_->text_input.observation()}}}}},
                {"nodeCount", nodes.size()}, {"nodes", std::move(nodes)},
                {"layoutDiagnostics", {{"overflowCount", overflow_nodes.size()}, {"overflowNodes", std::move(overflow_nodes)}}},
                {"logs", impl_->system.log_summary()}}.dump();
}

std::string RetainedUiRuntime::render_packet_json() const {
    return impl_->primary_packet_summary.is_object()?impl_->primary_packet_summary.dump():
        Json{{"schemaVersion","noemancer.ui-render-packet/0.1"},{"backend","capture"},
            {"drawCount",0},{"triangleCount",0},{"residentGeometryCount",0},
            {"drawSamples",Json::array()},{"drawsTruncated",false}}.dump();
}
RetainedUiRenderPacket RetainedUiRuntime::render_packet() const { return impl_->primary_packet; }
RetainedUiRenderPacket RetainedUiRuntime::surface_render_packet(const std::string_view surface_id) const {
    if(surface_id=="primary")return impl_->primary_packet;
    const auto found=impl_->surfaces.find(std::string(surface_id));return found==impl_->surfaces.end()?RetainedUiRenderPacket{}:found->second.packet;
}

std::string retained_ui_text_capabilities_json(const std::string_view locale, const std::string_view sample_text,
                                               const std::string_view font_path, const float font_size) {
    const auto candidates=available_font_candidates();
    const auto requested=std::string(locale);
    const auto script=requested.starts_with("zh")?"Han":requested.starts_with("ja")?"Japanese":
        requested.starts_with("ko")?"Hangul":requested.starts_with("ar")?"Arabic":
        requested.starts_with("he")?"Hebrew":"Latin";
    Json fonts=Json::array();
    for(const auto& candidate:candidates) fonts.push_back({{"id",candidate.id},{"path",candidate.path.generic_string()},
        {"scripts",candidate.scripts},{"available",true},{"source","platform-font"}});
    const auto coverage_script=script==std::string_view("Japanese")?std::string_view("Han"):std::string_view(script);
    const auto layout=Json::parse(text_layout_inspect_json({.text=std::string(sample_text),.locale=requested,
        .font_path=std::string(font_path),.font_size=font_size}));
    Json result{{"schemaVersion","noemancer.ui-text-capabilities/0.1"},{"valid",true},{"code","ok"},
        {"locale",requested},{"requiredScript",script},{"platformFallbackFaces",std::move(fonts)},
        {"requiredScriptFallbackAvailable",script==std::string_view("Latin")||covers_script(candidates,coverage_script)},
        {"fontSelection",{{"engine","RmlUi FreeType default"},{"fallbackFaces",true},{"platformResolved",true}}},
        {"textInput",{{"committedUtf8",true},{"sdlImeCandidatePlacement",true},{"compositionPreview",true}}},
        {"shaping",{{"harfBuzz",true},{"complexScripts",true},{"rendererNeutralGlyphRuns",true},
            {"version",layout.value("engines",Json::object()).value("harfBuzzVersion",std::string{})}}},
        {"segmentation",{{"icu",true},{"bidirectionalLayout",true},{"localeLineBreaking",true},
            {"version",layout.value("engines",Json::object()).value("icuVersion",std::string{})}}},
        {"retainedRenderIntegration",{{"glyphRunConsumer",true},{"defaultRmlUiRendererStillCodepointBased",false},
            {"atlasBackend","RmlUi FreeType engine patch"}}},
        {"nextIntegration","cache shaped runs and expose cluster-aware caret selection"}};
    if(!sample_text.empty()) result["layoutPlan"]=layout;
    else result["layoutProbe"]={{"valid",layout.value("valid",false)},{"code",layout.value("code",std::string{})},
        {"fontPath",layout.value("fontPath",std::string{})}};
    return result.dump();
}

std::string retained_ui_rml_from_semantic_document(const std::string_view source_json) {
    const auto source = Json::parse(source_json, nullptr, false);
    if (source.is_discarded() || !source.is_object() || source.value("schemaVersion", "") != "noemancer.ui-document/0.1") return {};
    const auto& nodes = source.value("nodes", Json::array());
    // RmlUi currently retains the complete document. This is an explicit
    // bounded-document contract, not a claim of virtualized list rendering.
    constexpr std::size_t maximum_retained_nodes=2048U;
    if(!nodes.is_array()||nodes.size()>maximum_retained_nodes)return {};
    std::unordered_set<std::string> node_ids;
    for(const auto& node:nodes) {
        if(!node.is_object())return {};
        const auto id=node.value("id",std::string{});
        if(id.empty()||!node_ids.insert(id).second)return {};
    }
    std::unordered_map<std::string, std::vector<Json>> children;
    std::vector<Json> roots;
    for (const auto& node : nodes) {
        if (node.contains("parentId") && node.at("parentId").is_string()) children[node.at("parentId").get<std::string>()].push_back(node);
        else roots.push_back(node);
    }
    const auto tokens=source.value("designTokens",Json::object());
    const auto default_grid_columns=bounded_grid_columns(tokens.value("gridColumns",Json(4)),4U);
    std::unordered_map<std::string,std::size_t> grid_columns;
    for(const auto& node:nodes)if(node.value("role",std::string{})=="grid") {
        const auto presentation=node.value("presentation",Json::object());
        grid_columns[node.value("id",std::string{})]=bounded_grid_columns(
            presentation.value("gridColumns",Json(default_grid_columns)),default_grid_columns);
    }
    const auto surface_color=tokens.value("surfaceColor",std::string("#11151cee"));
    const auto group_color=tokens.value("groupColor",std::string("#1a202a"));
    const auto text_color=tokens.value("textColor",std::string("#e8edf5"));
    const auto accent_color=tokens.value("accentColor",std::string("#95c8ff"));
    const auto surface_width=std::clamp(tokens.value("surfaceWidthPx",360),240,720);
    const auto embedded_surface=tokens.value("embeddedSurface",false);
    std::ostringstream output;
    output << "<rml><head><style>"
              "body{box-sizing:border-box;margin:0;width:100%;height:100%;background:transparent;color:" << escape_markup(text_color) << ";font-family:LatoLatin;font-size:14px;pointer-events:none;}"
              ".surface{box-sizing:border-box;display:flex;flex-direction:column;width:" << surface_width << "px;max-height:100%;padding:12px;background:" << escape_markup(surface_color) << ";overflow-y:auto;pointer-events:auto;}"
              ".hud-surface{position:absolute;right:20px;bottom:20px;}"
              ".group{box-sizing:border-box;display:flex;flex-direction:column;flex-shrink:0;width:100%;margin-bottom:8px;background:" << escape_markup(group_color) << ";border-width:1px;border-color:#273140;}"
              ".group-header{box-sizing:border-box;display:flex;flex-direction:row;align-items:center;width:100%;height:30px;padding:0 8px;background:#202733;color:" << escape_markup(text_color) << ";border-width:0;border-bottom-width:1px;border-color:#2c3747;text-align:left;}"
              ".group-header:hover{background:#273141;}.group-header:focus{background:#2b3749;border-color:" << escape_markup(accent_color) << ";}"
              ".group-chevron{display:block;width:18px;color:" << escape_markup(accent_color) << ";font-size:16px;text-align:center;}"
              ".group-title{margin-left:4px;font-size:13px;letter-spacing:0.4px;}"
              ".group-content{box-sizing:border-box;display:flex;flex-direction:column;width:100%;padding:6px 4px 7px 4px;}"
              ".group.collapsed .group-content{display:none;}.group.collapsed .group-header{border-bottom-width:0;}"
              ".property-row{box-sizing:border-box;display:flex;flex-direction:row;flex-shrink:0;justify-content:space-between;width:100%;min-height:28px;padding:4px 8px;}"
              ".property-row:hover{background:#222b38;}.property-row.disabled{opacity:0.48;}.property-row.error{background:#3a2025;border-left-width:2px;border-left-color:#ff6b78;}"
              ".selectable-collection{box-sizing:border-box;display:flex;flex-direction:column;width:100%;overflow-y:auto;pointer-events:auto;}"
              ".selectable-row{box-sizing:border-box;display:flex;flex-direction:row;flex-shrink:0;align-items:center;width:100%;min-height:28px;padding:4px 8px;pointer-events:auto;}"
              ".selectable-row:hover{background:#222b38;}.selectable-row:focus{background:#28384c;outline:1px " << escape_markup(accent_color) << ";}"
              ".selectable-row.selected{background:#294461;color:#ffffff;}.selectable-row.disabled{opacity:0.48;}"
              ".grid-collection{flex-direction:column;padding:4px;overflow-y:auto;}"
              ".grid-collection>.label{width:100%;padding:4px 5px;color:#91a0b4;}"
              ".grid-row{box-sizing:border-box;display:flex;flex-direction:row;flex-shrink:0;width:100%;}"
              ".selectable-card{box-sizing:border-box;display:flex;flex-direction:column;flex-shrink:0;min-width:0;min-height:92px;padding:7px;pointer-events:auto;background:#171e28;border-width:2px;border-color:transparent;}"
              ".selectable-card:hover{background:#202b39;}.selectable-card:focus{background:#223247;border-color:" << escape_markup(accent_color) << ";}"
              ".selectable-card.selected{background:#294461;border-color:" << escape_markup(accent_color) << ";color:#ffffff;}.selectable-card.disabled{opacity:0.48;}"
              ".selectable-card>.label{width:100%;margin-top:4px;color:" << escape_markup(text_color) << ";overflow:hidden;}"
              ".selectable-card>.value{width:100%;margin-top:2px;overflow:hidden;}"
              ".collection-card-image{box-sizing:border-box;width:100%;height:48px;background:#0c1118;border-width:1px;border-color:#253143;}"
              ".collection-card-metadata{display:flex;flex-direction:column;width:100%;margin-top:3px;color:#91a0b4;font-size:11px;}"
              ".collection-card-meta{width:100%;overflow:hidden;}"
              ".tree-row{flex-direction:column;align-items:stretch;min-height:0;padding:0;background:transparent;}"
              ".tree-row>.label{box-sizing:border-box;width:100%;min-height:28px;padding:5px 8px;pointer-events:auto;}"
              ".tree-row:hover,.tree-row:focus,.tree-row.selected{background:transparent;outline-width:0;}"
              ".tree-row:hover>.label{background:#222b38;}.tree-row:focus>.label{background:#28384c;outline:1px " << escape_markup(accent_color) << ";}"
              ".tree-row.selected>.label{background:#294461;color:#ffffff;}.tree-row>.tree-row{padding-left:14px;}"
              ".label,.value{min-width:0;word-break:break-word;}"
              ".label{width:46%;}.value{width:50%;color:" << escape_markup(accent_color) << ";}"
              ".value-editor{box-sizing:border-box;width:50%;min-width:0;padding:3px 6px;background:#080b10;color:" << escape_markup(accent_color) << ";border-width:1px;border-color:#34445a;}"
              ".value-editor:hover{border-color:#57708f;}.value-editor:focus{border-color:" << escape_markup(accent_color) << ";}"
              ".value-editor:disabled{background:#151922;color:#697385;border-color:#2b3442;}"
              ".vector-editor{box-sizing:border-box;display:flex;flex-direction:row;width:50%;min-width:0;}"
              ".axis-field{box-sizing:border-box;display:flex;flex-direction:row;flex-grow:1;min-width:0;margin-left:3px;background:#080b10;border-width:1px;border-color:#34445a;}"
              ".axis-field:first-child{margin-left:0;}.axis-field:hover{border-color:#57708f;}.axis-label{width:14px;padding-top:4px;color:#75849a;text-align:center;font-size:11px;}"
              ".axis-input{box-sizing:border-box;display:block;flex-grow:1;min-width:0;width:auto;height:24px;padding:2px 3px;background:transparent;color:" << escape_markup(accent_color) << ";border-width:0;}"
              ".axis-input:focus{background:#111925;color:#d9ecff;}"
              ".value-editor.checkbox{width:18px;height:18px;margin-left:auto;padding:0;background:#080b10;border-width:1px;border-color:#4b607b;}"
              ".value-editor.checkbox:checked{background:" << escape_markup(accent_color) << ";border-color:" << escape_markup(accent_color) << ";}"
              ".value-editor.range{height:22px;padding:0;background:transparent;border-width:0px;}"
              "input.range slidertrack{height:4px;margin-top:9px;background:#26364c;}"
              "input.range sliderprogress{height:4px;margin-top:9px;background:" << escape_markup(accent_color) << ";}"
              "input.range sliderbar{width:10px;height:16px;margin-left:-5px;margin-top:-6px;background:" << escape_markup(accent_color) << ";border-radius:5px;}"
              "input.range sliderbar:hover{background:#d4eaff;}input.range sliderarrowdec,input.range sliderarrowinc{display:none;}"
              "scrollbarvertical{width:9px;background:#111720;}scrollbarvertical slidertrack{background:#111720;}"
              "scrollbarvertical sliderbar{min-height:28px;background:#37465a;border-radius:4px;}scrollbarvertical sliderbar:hover{background:#526984;}"
              "scrollbarvertical sliderarrowdec,scrollbarvertical sliderarrowinc{display:none;}"
              ".meter{display:flex;flex-direction:column;margin:4px 0;}.meter .label{width:100%;}"
              ".meter-line{display:flex;flex-direction:row;align-items:center;margin-top:3px;}"
              ".meter-track{width:210px;height:8px;background:#080b10;border-radius:4px;overflow:hidden;}"
              ".meter-fill{display:block;height:8px;background:" << escape_markup(accent_color) << ";}.meter-number{margin-left:8px;color:" << escape_markup(text_color) << ";}"
              ".ability-slot{margin-top:6px;background:" << escape_markup(group_color) << ";border-left-width:3px;border-left-color:" << escape_markup(accent_color) << ";}"
              ".action-button{box-sizing:border-box;width:100%;min-height:34px;margin-top:6px;padding:6px 10px;background:" << escape_markup(group_color) << ";color:" << escape_markup(text_color) << ";border-width:1px;border-color:#34445a;text-align:center;pointer-events:auto;}"
              ".action-button:hover{background:#273449;border-color:" << escape_markup(accent_color) << ";}.action-button:focus{border-color:" << escape_markup(accent_color) << ";}.action-button:disabled{opacity:0.48;}"
              << source.value("resources",Json::object()).value("stylesheet",Json::object()).value("content",std::string{}) <<
              "</style></head><body dir=\"" << escape_markup(source.value("textDirection",std::string("ltr"))) <<
              "\" lang=\"" << escape_markup(source.value("locale",std::string("en-US"))) << "\">";
    const auto emit = [&](const auto& self, const Json& node) -> void {
        const auto id = node.value("id", "");
        const auto role = node.value("role", "node");
        const auto state=node.value("state",Json::object());
        const auto enabled=state.value("enabled",true);
        const auto editable=state.value("editable",false);
        const auto expanded=state.value("expanded",true);
        const auto selected=state.value("selected",false);
        const auto selectable=selectable_row_role(role);
        const auto grid_item=role=="grid-item"||role=="griditem";
        const auto parent_id=node.contains("parentId")&&node.at("parentId").is_string()?
            node.at("parentId").get<std::string>():std::string{};
        const auto grid_column_count=grid_columns.contains(parent_id)?grid_columns.at(parent_id):default_grid_columns;
        const auto presentation=node.value("presentation",Json::object());
        auto image_source=presentation.value("imageSource",std::string{});
        if(image_source.size()>1024U)image_source.clear();
        Json compact_metadata=Json::object();
        if(const auto metadata=node.find("metadata");metadata!=node.end()&&metadata->is_object()) {
            std::size_t count{};
            for(auto field=metadata->begin();field!=metadata->end()&&count<8U;++field) {
                if(field.value().is_string())compact_metadata[field.key()]=field.value().get<std::string>().substr(0U,256U);
                else if(field.value().is_boolean()||field.value().is_number()||field.value().is_null())compact_metadata[field.key()]=field.value();
                else continue;
                ++count;
            }
        }
        std::string error;
        if(state.contains("error"))error=state.at("error").is_string()?state.at("error").get<std::string>():state.at("error").dump();
        auto classes=role_class(role);
        if(!enabled)classes+=" disabled";
        if(!error.empty())classes+=" error";
        if(role=="group"&&!expanded)classes+=" collapsed";
        if(selectable&&selected)classes+=" selected";
        const auto actions=node.value("actions",Json::array());
        const auto has_action=actions.is_array()&&!actions.empty()&&actions.front().is_object();
        const auto action_binding=has_action&&actions.front().contains("binding")?
            actions.front().at("binding"):node.value("binding",Json::object());
        const auto action_button=role=="button";
        output << (action_button?"<button type=\"button\"":"<div") << " id=\"" << escape_markup(id) << "\" data-semantic-id=\"" << escape_markup(id)
               << "\" data-role=\"" << escape_markup(role) << "\" data-enabled=\"" << (enabled?"true":"false")
               << "\" data-editable=\"" << (editable?"true":"false") << "\"";
        if(action_button&&!enabled)output << " disabled";
        if(role=="group")output << " data-expanded=\"" << (expanded?"true":"false") << "\"";
        if(role=="grid")output << " data-grid-columns=\"" << grid_columns.at(id) << "\"";
        if(selectable)output << " data-selected=\"" << (selected?"true":"false")
                             << "\" aria-selected=\"" << (selected?"true":"false")
                             << "\" tabindex=\"0\"";
        if(grid_item)output << " style=\"width:" << compact_number(100.0/static_cast<double>(grid_column_count)) << "%\"";
        if(!image_source.empty())output << " data-image-source=\"" << escape_markup(image_source) << "\"";
        if(!compact_metadata.empty())output << " data-metadata=\"" << escape_markup(compact_metadata.dump()) << "\"";
        if(!error.empty())output << " data-error=\"" << escape_markup(error) << "\"";
        if(action_binding.is_object()&&!action_binding.empty())
            output << " data-binding=\"" << escape_markup(action_binding.dump()) << "\"";
        if(has_action)output << " data-action=\"" << escape_markup(actions.front().value("id", "")) << "\"";
        output << " class=\"" << classes << "\">";
        if(role=="group") {
            output << "<button class=\"group-header\" data-local-action=\"toggle-group\" type=\"button\"><span class=\"group-chevron\">"
                   << (expanded?"&#8722;":"+") << "</span><span class=\"group-title\">" << escape_markup(node.value("label",id))
                   << "</span></button><div class=\"group-content\">";
            if(const auto found=children.find(id);found!=children.end())for(const auto& child:found->second)self(self,child);
            output << "</div></div>";
            return;
        }
        if(grid_item&&!image_source.empty())
            output << "<img class=\"collection-card-image\" data-image-source=\"" << escape_markup(image_source)
                   << "\" src=\"" << escape_markup(image_source) << "\" alt=\""
                   << escape_markup(node.value("label",id)) << "\"/>";
        if(!(embedded_surface&&role=="inspector"))
            output << "<span class=\"label\">" << escape_markup(node.value("label", id)) << "</span>";
        if (role=="meter" && node.contains("value") && node.at("value").is_object()) {
            const auto current=node.at("value").value("current",0.0);
            const auto maximum=node.at("value").value("maximum",1.0);
            const auto percent=std::clamp(node.at("value").value("normalized",0.0),0.0,1.0)*100.0;
            output << "<div class=\"meter-line\"><div class=\"meter-track\"><div class=\"meter-fill\" style=\"width:"
                   << percent << "%\"></div></div><span class=\"meter-number\">" << current << "/" << maximum << "</span></div>";
        } else if (role=="status" && node.contains("value") && node.at("value").is_object()) {
            std::ostringstream value;
            const auto tags=node.at("value").value("tags",Json::array());
            for (std::size_t index=0;index<tags.size();++index) {
                if (index) value << ", ";
                value << tags[index].get<std::string>();
            }
            output << "<span class=\"value\">" << escape_markup(value.str().empty()?"Ready":value.str()) << "</span>";
        } else if (role=="ability-slot" && node.contains("value") && node.at("value").is_object()) {
            const auto cooldown=node.at("value").value("cooldownSeconds",0.0);
            output << "<span class=\"value\">" << (cooldown>0.0?std::to_string(cooldown)+"s":"Ready") << "</span>";
        } else if (node.contains("value") && !node.at("value").is_null()) {
            const auto value_type=node.value("binding",Json::object()).value("valueType",std::string{});
            const auto value_presentation=node.value("presentation",Json::object());
            const auto control=value_presentation.value("control",std::string{});
            const auto constraints=value_presentation.value("constraints",Json::object());
            const auto value=node.at("value").is_string()?node.at("value").get<std::string>():
                node.at("value").is_number()?compact_number(node.at("value").get<double>()):node.at("value").dump();
            if(editable&&control=="checkbox"&&node.at("value").is_boolean())
                output << "<input id=\"" << escape_markup(id+".editor") << "\" class=\"value-editor checkbox\" type=\"checkbox\" value=\"true\" "
                       << (node.at("value").get<bool>()?"checked ":"") << (!enabled?"disabled ":"") << "data-owner-semantic-id=\"" << escape_markup(id) << "\"/>";
            else if(editable&&control=="combo"&&constraints.value("options",Json::array()).is_array()) {
                output << "<select id=\"" << escape_markup(id+".editor") << "\" class=\"value-editor\" data-owner-semantic-id=\""
                       << escape_markup(id) << "\"" << (!enabled?" disabled":"") << ">";
                for(const auto& option:constraints.value("options",Json::array()))if(option.is_string()) {
                    const auto option_value=option.get<std::string>();output << "<option value=\"" << escape_markup(option_value) << "\""
                        << (option_value==value?" selected":"") << ">" << escape_markup(option_value) << "</option>";
                }
                output << "</select>";
            } else if(editable&&control=="slider"&&node.at("value").is_number()) {
                output << "<input id=\"" << escape_markup(id+".editor") << "\" class=\"value-editor range\" type=\"range\" value=\""
                       << escape_markup(value) << "\" min=\"" << constraints.value("minimum",0.0) << "\" max=\""
                       << constraints.value("maximum",1.0) << "\" step=\"" << constraints.value("step",0.01) << "\" data-owner-semantic-id=\""
                       << escape_markup(id) << "\"" << (!enabled?" disabled":"") << "/>";
            } else if(editable&&(value_type=="vector3"||value_type=="color3")&&node.at("value").is_object()) {
                output << "<div class=\"vector-editor\">";
                for(const auto axis:{"x","y","z"}) {
                    const auto axis_value=compact_number(node.at("value").value(axis,0.0));
                    output << "<label class=\"axis-field\"><span class=\"axis-label\">" << static_cast<char>(std::toupper(axis[0]))
                           << "</span><input id=\"" << escape_markup(id+".editor."+axis) << "\" class=\"axis-input vector-" << axis
                           << "\" type=\"text\" value=\"" << axis_value << "\" data-owner-semantic-id=\"" << escape_markup(id) << "\""
                           << (!enabled?" disabled":"") << "/></label>";
                }
                output << "</div>";
            } else if(editable&&(value_type=="string"||value_type=="asset-id"||value_type=="f32"||value_type=="f64"||
                       value_type=="i32"||value_type=="u32"))
                output << "<input id=\"" << escape_markup(id+".editor") << "\" class=\"value-editor\" type=\"text\" value=\""
                       << escape_markup(value) << "\" data-owner-semantic-id=\"" << escape_markup(id) << "\"" << (!enabled?" disabled":"") << "/>";
            else output << "<span class=\"value\">" << escape_markup(value) << "</span>";
        }
        if(grid_item&&!compact_metadata.empty()) {
            output << "<div class=\"collection-card-metadata\">";
            for(auto field=compact_metadata.begin();field!=compact_metadata.end();++field) {
                const auto value=field.value().is_string()?field.value().get<std::string>():field.value().dump();
                output << "<span class=\"collection-card-meta\">" << escape_markup(field.key()) << ": "
                       << escape_markup(value) << "</span>";
            }
            output << "</div>";
        }
        if(role=="grid") {
            if(const auto found=children.find(id);found!=children.end()) {
                for(std::size_t index=0;index<found->second.size();++index) {
                    if(index%grid_columns.at(id)==0U)output << "<div class=\"grid-row\">";
                    self(self,found->second[index]);
                    if(index%grid_columns.at(id)+1U==grid_columns.at(id)||index+1U==found->second.size())output << "</div>";
                }
            }
            output << "</div>";
            return;
        }
        if (const auto found = children.find(id); found != children.end()) for (const auto& child : found->second) self(self, child);
        output << (action_button?"</button>":"</div>");
    };
    for (const auto& root : roots) emit(emit, root);
    output << "</body></rml>";
    return output.str();
}

std::string retained_ui_preview_json(const std::string_view source, const std::uint32_t width,
                                     const std::uint32_t height, const float density_scale) {
    const auto parsed = Json::parse(source, nullptr, false);
    const auto document_id = parsed.is_object() ? parsed.value("documentId", "ui.preview") : std::string("ui.preview");
    const auto rml = retained_ui_rml_from_semantic_document(source);
    RetainedUiRuntime runtime;
    if (rml.empty() || !runtime.initialize(width, height, density_scale) || !runtime.load_document(document_id, rml) || !runtime.render())
        return Json{{"schemaVersion", "noemancer.retained-ui-preview/0.1"}, {"valid", false},
                    {"code", "ui.retained-preview-failed"}, {"detail", runtime.last_error()}}.dump();
    return Json{{"schemaVersion", "noemancer.retained-ui-preview/0.1"}, {"valid", true}, {"code", "ok"},
                {"observation", Json::parse(runtime.observation_json(document_id))},
                {"renderPacket", Json::parse(runtime.render_packet_json())}}.dump();
}

} // namespace noemancer
