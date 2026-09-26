#include <Renderer/UI/Docking/DockTypes.hpp>

#include <charconv>
#include <cstdio>
#include <sstream>


namespace SFT::UI::Docking {

    /// Reports whether contains holds for this `Docking`.
    ///
    /// @param point `point` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    bool DockRect::contains(glm::vec2 point) const noexcept {
        return point.x >= origin.x && point.x <= origin.x + size.x && point.y >= origin.y &&
               point.y <= origin.y + size.y;
    }

    /// Returns the current or globally available root value.
    ///
    /// @return Returns the current root value.
    /// @note This function does not throw exceptions.
    DockNodeId DockTree::root() const noexcept { return root_; }

    /// Performs the node operation for `Docking` using the supplied arguments.
    ///
    /// @param id Identifier of the target object or resource.
    ///
    /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
    /// @note This function does not throw exceptions.
    const DockNode *DockTree::node(DockNodeId id) const noexcept {
        const u32 index = static_cast<u32>(id);
        return index < nodes_.size() && nodes_[index].has_value() ? &*nodes_[index] : nullptr;
    }

    /// Performs the merge into leaf operation for `Docking` using the supplied arguments.
    ///
    /// @param target `target` value used by the operation.
    /// @param panel `panel` value used by the operation.
    /// @param before `before` value used by the operation.
    ///
    /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
    bool DockTree::merge_into_leaf(DockNodeId target, DockPanelId panel,
                                       optional<usize> before) {
        DockNode *n = mutable_node(target);
        if (!n || n->kind != DockNode::Kind::Leaf || find_leaf_of(panel)) {
            return false;
        }
        const usize insert_at = before && *before <= n->tabs.size() ? *before : n->tabs.size();
        n->tabs.insert(n->tabs.begin() + static_cast<isize>(insert_at), panel);
        n->active_tab_index = insert_at;
        return true;
    }

    /// Splits leaf using the supplied arguments and current state.
    ///
    /// @param target `target` value used by the operation.
    /// @param axis `axis` value used by the operation.
    /// @param panel_first `panel_first` value used by the operation.
    /// @param panel `panel` value used by the operation.
    /// @param new_panel_ratio `new_panel_ratio` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    bool DockTree::split_leaf(DockNodeId target, DockSplitAxis axis, bool panel_first,
                                  DockPanelId panel, f32 new_panel_ratio) {
        DockNode *target_before = mutable_node(target);
        if (!target_before || target_before->kind != DockNode::Kind::Leaf || find_leaf_of(panel)) {
            return false;
        }
        DockNode existing_leaf{};
        existing_leaf.kind = DockNode::Kind::Leaf;
        existing_leaf.tabs = std::move(target_before->tabs);
        existing_leaf.active_tab_index = target_before->active_tab_index;

        DockNode new_leaf{};
        new_leaf.kind = DockNode::Kind::Leaf;
        new_leaf.tabs = {std::move(panel)};
        new_leaf.active_tab_index = 0;


        const DockNodeId existing_id = alloc_node(std::move(existing_leaf));
        const DockNodeId new_id = alloc_node(std::move(new_leaf));

        DockNode *target_after = mutable_node(target);
        target_after->kind = DockNode::Kind::Split;
        target_after->split_axis = axis;
        const f32 clamped_new_panel_ratio = std::clamp(new_panel_ratio, 0.0f, 1.0f);
        target_after->split_ratio =
            panel_first ? clamped_new_panel_ratio : (1.0f - clamped_new_panel_ratio);
        target_after->first_child = panel_first ? new_id : existing_id;
        target_after->second_child = panel_first ? existing_id : new_id;
        target_after->tabs.clear();
        return true;
    }

    /// Removes the panel from its owning collection or system.
    ///
    /// @param panel `panel` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void DockTree::remove_panel(const DockPanelId &panel) {
        const optional<DockNodeId> leaf_id = find_leaf_of(panel);
        if (!leaf_id) {
            return;
        }
        DockNode *leaf = mutable_node(*leaf_id);
        const auto it = std::find(leaf->tabs.begin(), leaf->tabs.end(), panel);
        if (it == leaf->tabs.end()) {
            return;
        }
        const usize removed_index = static_cast<usize>(it - leaf->tabs.begin());
        leaf->tabs.erase(it);
        if (leaf->tabs.empty()) {
            leaf->active_tab_index = 0;
        } else if (removed_index < leaf->active_tab_index) {
            leaf->active_tab_index -= 1;
        } else if (leaf->active_tab_index >= leaf->tabs.size()) {
            leaf->active_tab_index = leaf->tabs.size() - 1;
        }
        if (!leaf->tabs.empty() || *leaf_id == root()) {
            return;
        }
        collapse_empty_leaf(*leaf_id);
    }

    /// Finds leaf of in the available state.
    ///
    /// @param panel `panel` value used by the operation.
    ///
    /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
    /// @note This function does not throw exceptions.
    optional<DockNodeId> DockTree::find_leaf_of(const DockPanelId &panel) const noexcept {
        return find_leaf_of_from(panel, root());
    }

    /// Reports whether this `Docking` contains no elements or payload.
    ///
    /// @return Returns the current empty value.
    /// @note This function does not throw exceptions.
    bool DockTree::empty() const noexcept {
        const DockNode *r = node(root());
        return r != nullptr && r->kind == DockNode::Kind::Leaf && r->tabs.empty();
    }

    /// Sets the split ratio for this `Docking`.
    ///
    /// @param id Identifier of the target object or resource.
    /// @param ratio `ratio` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void DockTree::set_split_ratio(DockNodeId id, f32 ratio) noexcept {
        DockNode *n = mutable_node(id);
        if (n != nullptr && n->kind == DockNode::Kind::Split) {
            n->split_ratio = std::clamp(ratio, 0.0f, 1.0f);
        }
    }

    /// Sets the active tab for this `Docking`.
    ///
    /// @param leaf `leaf` value used by the operation.
    /// @param panel `panel` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void DockTree::set_active_tab(DockNodeId leaf, const DockPanelId &panel) noexcept {
        DockNode *n = mutable_node(leaf);
        if (n == nullptr || n->kind != DockNode::Kind::Leaf) {
            return;
        }
        for (usize i = 0; i < n->tabs.size(); ++i) {
            if (n->tabs[i] == panel) {
                n->active_tab_index = i;
                return;
            }
        }
    }

    /// Performs the reorder tab operation for `Docking` using the supplied arguments.
    ///
    /// @param leaf `leaf` value used by the operation.
    /// @param from_index Zero-based index of the target element or entry.
    /// @param to_index Zero-based index of the target element or entry.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void DockTree::reorder_tab(DockNodeId leaf, usize from_index, usize to_index) noexcept {
        DockNode *n = mutable_node(leaf);
        if (n == nullptr || n->kind != DockNode::Kind::Leaf) {
            return;
        }
        if (from_index >= n->tabs.size() || to_index >= n->tabs.size() || from_index == to_index) {
            return;
        }
        const DockPanelId moved = n->tabs[from_index];
        n->tabs.erase(n->tabs.begin() + static_cast<isize>(from_index));
        n->tabs.insert(n->tabs.begin() + static_cast<isize>(to_index), moved);
        n->active_tab_index = to_index;
    }

    /// Performs the mutable node operation for `Docking` using the supplied arguments.
    ///
    /// @param id Identifier of the target object or resource.
    ///
    /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
    /// @note This function does not throw exceptions.
    DockNode *DockTree::mutable_node(DockNodeId id) noexcept {
        const u32 index = static_cast<u32>(id);
        return index < nodes_.size() && nodes_[index].has_value() ? &*nodes_[index] : nullptr;
    }

    /// Performs the alloc node operation for `Docking` using the supplied arguments.
    ///
    /// @param value Value consumed by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    DockNodeId DockTree::alloc_node(DockNode value) {
        if (!free_list_.empty()) {
            const u32 index = free_list_.back();
            free_list_.pop_back();
            nodes_[index] = std::move(value);
            return static_cast<DockNodeId>(index);
        }
        nodes_.push_back(std::move(value));
        return static_cast<DockNodeId>(nodes_.size() - 1);
    }

    /// Releases previously allocated storage or resources.
    ///
    /// @param id Identifier of the target object or resource.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void DockTree::free_node(DockNodeId id) {
        const u32 index = static_cast<u32>(id);
        if (index < nodes_.size()) {
            nodes_[index].reset();
            free_list_.push_back(index);
        }
    }

    /// Finds leaf of from in the available state.
    ///
    /// @param panel `panel` value used by the operation.
    /// @param from `from` value used by the operation.
    ///
    /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
    /// @note Normal inability to produce a value is represented by an empty optional.
    /// @note This function does not throw exceptions.
    optional<DockNodeId> DockTree::find_leaf_of_from(const DockPanelId &panel, DockNodeId from) const noexcept {
        const DockNode *n = node(from);
        if (n == nullptr) {
            return std::nullopt;
        }
        if (n->kind == DockNode::Kind::Leaf) {
            for (const DockPanelId &tab : n->tabs) {
                if (tab == panel) {
                    return from;
                }
            }
            return std::nullopt;
        }
        if (const optional<DockNodeId> first = find_leaf_of_from(panel, n->first_child)) {
            return first;
        }
        return find_leaf_of_from(panel, n->second_child);
    }

    /// Finds parent in the available state.
    ///
    /// @param child `child` value used by the operation.
    /// @param from `from` value used by the operation.
    ///
    /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
    /// @note Normal inability to produce a value is represented by an empty optional.
    /// @note This function does not throw exceptions.
    optional<DockTree::ParentLink> DockTree::find_parent(DockNodeId child, DockNodeId from) const noexcept {
        const DockNode *n = node(from);
        if (n == nullptr || n->kind != DockNode::Kind::Split) {
            return std::nullopt;
        }
        if (n->first_child == child) {
            return ParentLink{from, true};
        }
        if (n->second_child == child) {
            return ParentLink{from, false};
        }
        if (const optional<ParentLink> first = find_parent(child, n->first_child)) {
            return first;
        }
        return find_parent(child, n->second_child);
    }

    /// Performs the collapse empty leaf operation for `Docking` using the supplied arguments.
    ///
    /// @param leaf_id Identifier of the target object or resource.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void DockTree::collapse_empty_leaf(DockNodeId leaf_id) {
        const optional<ParentLink> link = find_parent(leaf_id, root());
        if (!link) {
            return;
        }
        const DockNode *parent = node(link->parent);
        const DockNodeId sibling = link->is_first ? parent->second_child : parent->first_child;
        const optional<ParentLink> grandparent = find_parent(link->parent, root());

        free_node(leaf_id);
        free_node(link->parent);

        if (grandparent) {
            DockNode *gp = mutable_node(grandparent->parent);
            (grandparent->is_first ? gp->first_child : gp->second_child) = sibling;
        } else {


            root_ = sibling;
        }
    }

} // namespace SFT::UI::Docking


namespace SFT::UI::Docking {

    /// Performs the dock tree operation for `Docking` using the supplied arguments.
    ///
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    DockTree::DockTree() { nodes_.push_back(DockNode{}); }

    bool dock_layout_is_valid(const DockLayoutSnapshot &snapshot) noexcept {
        const usize count = snapshot.nodes.size();
        if (count == 0) {
            return false;
        }
        // Every node except the root must be the child of exactly one split, and following children from
        // the root must reach each node exactly once: that rules out cycles, sharing and orphans.
        vector<u8> visits(count, 0);
        vector<u32> pending{0};
        visits[0] = 1;
        while (!pending.empty()) {
            const u32 index = pending.back();
            pending.pop_back();
            const DockLayoutNode &node = snapshot.nodes[index];
            if (node.is_leaf) {
                if (!node.tabs.empty() && node.active_tab_index >= node.tabs.size()) {
                    return false;
                }
                continue;
            }
            for (const u32 child : {node.first_child, node.second_child}) {
                if (child >= count || visits[child] != 0) {
                    return false;
                }
                visits[child] = 1;
                pending.push_back(child);
            }
        }
        for (const u8 visited : visits) {
            if (visited == 0) {
                return false;
            }
        }
        return !snapshot.focused_leaf || (*snapshot.focused_leaf < count && snapshot.nodes[*snapshot.focused_leaf].is_leaf);
    }

    DockLayoutSnapshot DockTree::snapshot() const {
        DockLayoutSnapshot result;
        vector<std::pair<DockNodeId, u32>> pending; // live id -> snapshot index of an already-allocated slot
        result.nodes.emplace_back();
        pending.emplace_back(root_, 0);
        while (!pending.empty()) {
            const auto [live_id, slot] = pending.back();
            pending.pop_back();
            const DockNode *live = node(live_id);
            DockLayoutNode out;
            if (live == nullptr) {
                result.nodes[slot] = std::move(out);
                continue;
            }
            if (live->kind == DockNode::Kind::Leaf) {
                out.is_leaf = true;
                out.tabs = live->tabs;
                out.active_tab_index = live->active_tab_index;
            } else {
                out.is_leaf = false;
                out.split_axis = live->split_axis;
                out.split_ratio = live->split_ratio;
                out.first_child = static_cast<u32>(result.nodes.size());
                result.nodes.emplace_back();
                out.second_child = static_cast<u32>(result.nodes.size());
                result.nodes.emplace_back();
                pending.emplace_back(live->first_child, out.first_child);
                pending.emplace_back(live->second_child, out.second_child);
            }
            result.nodes[slot] = std::move(out);
        }
        return result;
    }

    bool DockTree::restore(const DockLayoutSnapshot &layout) {
        if (!dock_layout_is_valid(layout)) {
            return false;
        }
        DockTree rebuilt;
        rebuilt.nodes_.clear();
        rebuilt.free_list_.clear();
        // Snapshot indices become live ids one-to-one, which keeps the root at id 0.
        for (const DockLayoutNode &source : layout.nodes) {
            DockNode live;
            if (source.is_leaf) {
                live.kind = DockNode::Kind::Leaf;
                live.tabs = source.tabs;
                live.active_tab_index = source.active_tab_index;
            } else {
                live.kind = DockNode::Kind::Split;
                live.split_axis = source.split_axis;
                live.split_ratio = std::clamp(source.split_ratio, 0.0f, 1.0f);
                live.first_child = static_cast<DockNodeId>(source.first_child);
                live.second_child = static_cast<DockNodeId>(source.second_child);
            }
            rebuilt.nodes_.push_back(std::move(live));
        }
        rebuilt.root_ = static_cast<DockNodeId>(0);
        *this = std::move(rebuilt);
        return true;
    }

    namespace {

        [[nodiscard]] std::string encode_panel_id(std::string_view id) {
            std::string out;
            for (const char c : id) {
                const auto byte = static_cast<unsigned char>(c);
                const bool plain = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9') || c == '.' || c == '_' || c == '-';
                if (plain) {
                    out.push_back(c);
                } else {
                    char escaped[4];
                    std::snprintf(escaped, sizeof(escaped), "%%%02X", byte);
                    out += escaped;
                }
            }
            return out;
        }

        [[nodiscard]] optional<std::string> decode_panel_id(std::string_view text) {
            std::string out;
            for (usize i = 0; i < text.size(); ++i) {
                if (text[i] != '%') {
                    out.push_back(text[i]);
                    continue;
                }
                if (i + 2 >= text.size()) {
                    return std::nullopt;
                }
                unsigned value = 0;
                const auto [ptr, ec] = std::from_chars(text.data() + i + 1, text.data() + i + 3, value, 16);
                if (ec != std::errc{} || ptr != text.data() + i + 3) {
                    return std::nullopt;
                }
                out.push_back(static_cast<char>(value));
                i += 2;
            }
            return out;
        }

    } // namespace

    std::string serialize_dock_layout(const DockLayoutSnapshot &snapshot) {
        std::ostringstream out;
        out << "sturdy-dock-layout 1\n";
        out << "focused " << (snapshot.focused_leaf ? static_cast<long long>(*snapshot.focused_leaf) : -1LL) << '\n';
        for (const DockLayoutNode &node : snapshot.nodes) {
            if (node.is_leaf) {
                out << "leaf " << node.active_tab_index;
                for (const DockPanelId &tab : node.tabs) {
                    out << ' ' << encode_panel_id(tab.cpp_string_view());
                }
                out << '\n';
            } else {
                char ratio[32];
                std::snprintf(ratio, sizeof(ratio), "%.9g", static_cast<double>(node.split_ratio));
                out << "split " << (node.split_axis == DockSplitAxis::Horizontal ? 'h' : 'v') << ' ' << ratio << ' ' << node.first_child << ' '
                    << node.second_child << '\n';
            }
        }
        return out.str();
    }

    optional<DockLayoutSnapshot> parse_dock_layout(std::string_view text) {
        std::istringstream in{std::string{text}};
        std::string line;
        if (!std::getline(in, line) || line != "sturdy-dock-layout 1") {
            return std::nullopt;
        }
        DockLayoutSnapshot result;
        if (!std::getline(in, line) || line.rfind("focused ", 0) != 0) {
            return std::nullopt;
        }
        {
            long long focused = 0;
            const std::string value = line.substr(8);
            const auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), focused);
            if (ec != std::errc{} || ptr != value.data() + value.size()) {
                return std::nullopt;
            }
            if (focused >= 0) {
                result.focused_leaf = static_cast<u32>(focused);
            }
        }
        while (std::getline(in, line)) {
            if (line.empty()) {
                continue;
            }
            std::istringstream fields{line};
            std::string kind;
            fields >> kind;
            DockLayoutNode node;
            if (kind == "leaf") {
                node.is_leaf = true;
                if (!(fields >> node.active_tab_index)) {
                    return std::nullopt;
                }
                std::string token;
                while (fields >> token) {
                    auto decoded = decode_panel_id(token);
                    if (!decoded) {
                        return std::nullopt;
                    }
                    node.tabs.emplace_back(std::string_view{*decoded});
                }
            } else if (kind == "split") {
                char axis = 0;
                double ratio = 0.0;
                if (!(fields >> axis >> ratio >> node.first_child >> node.second_child) || (axis != 'h' && axis != 'v')) {
                    return std::nullopt;
                }
                node.is_leaf = false;
                node.split_axis = axis == 'h' ? DockSplitAxis::Horizontal : DockSplitAxis::Vertical;
                node.split_ratio = static_cast<f32>(ratio);
            } else {
                return std::nullopt;
            }
            result.nodes.push_back(std::move(node));
        }
        if (!dock_layout_is_valid(result)) {
            return std::nullopt;
        }
        return result;
    }

} // namespace SFT::UI::Docking

