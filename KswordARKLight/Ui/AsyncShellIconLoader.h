#pragma once

#include "../Core/Win32Lean.h"
#include <commctrl.h>
#include <shellapi.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Ksword::Ui {

// Paint callbacks enqueue immutable icon inputs and immediately use a generic
// icon. Only the UI consumes results and mutates HIMAGELIST. Read-only workers
// own every HICON until UI consumption or cancellation; closing never joins.
class AsyncShellIconLoader final {
public:
    struct Source { std::wstring path; std::wstring identity; };
    using Resolver = std::function<Source()>;
    AsyncShellIconLoader(HIMAGELIST images, HWND owner, UINT message)
        : images_(images), state_(std::make_shared<State>(owner, message)) {
        if (images_) ImageList_AddIcon(images_, ::LoadIconW(nullptr, IDI_APPLICATION));
    }
    ~AsyncShellIconLoader() {
        std::lock_guard lock(state_->mutex);
        state_->owner = nullptr;
        state_->alive = false;
        state_->queue.clear();
        state_->ready.clear();
    }
    AsyncShellIconLoader(const AsyncShellIconLoader&) = delete;
    AsyncShellIconLoader& operator=(const AsyncShellIconLoader&) = delete;

    int indexFor(const std::wstring& key, Resolver resolver, ULONGLONG revalidateMs = 0) {
        const auto now = ::GetTickCount64();
        auto found = entries_.find(key);
        const int fallback = images_ ? 0 : -1;
        int index = fallback;
        bool request = found == entries_.end();
        if (found != entries_.end()) {
            found->second.used = ++useSerial_;
            index = found->second.index;
            request = revalidateMs != 0 && now - found->second.validated >= revalidateMs;
        }
        if (request && images_) enqueue(key, std::move(resolver));
        return index;
    }

    std::vector<std::wstring> consume(const WPARAM identity) {
        if (static_cast<std::uintptr_t>(identity) != state_->identity) return {};
        std::vector<Result> ready;
        {
            std::lock_guard lock(state_->mutex);
            ready.swap(state_->ready);
            for (const auto& result : ready) state_->pending.erase(result.key);
        }
        std::vector<std::wstring> changed;
        for (auto& result : ready) {
            auto found = entries_.find(result.key);
            int slot = found == entries_.end() ? -1 : found->second.index;
            if (found == entries_.end() && entries_.size() >= kMaximumEntries) {
                const auto oldest = std::min_element(entries_.begin(), entries_.end(),
                    [](const auto& a, const auto& b) { return a.second.used < b.second.used; });
                slot = oldest->second.index;
                changed.push_back(oldest->first);
                entries_.erase(oldest);
            }
            // Slot 0 is a permanent fallback and must never be overwritten.
            if (result.icon && result.icon->value) {
                if (slot > 0) {
                    ImageList_ReplaceIcon(images_, slot, result.icon->value);
                } else if (ImageList_GetImageCount(images_) < static_cast<int>(kMaximumEntries + 1)) {
                    slot = ImageList_AddIcon(images_, result.icon->value);
                }
            } else if (slot > 0) {
                ImageList_ReplaceIcon(images_, slot, ::LoadIconW(nullptr, IDI_APPLICATION));
            }
            if (slot < 0) slot = 0;
            entries_[result.key] = { slot, ++useSerial_, ::GetTickCount64(), std::move(result.identity) };
            changed.push_back(std::move(result.key));
        }
        return changed;
    }

private:
    static constexpr std::size_t kMaximumEntries = 256;
    struct Icon { HICON value = nullptr; ~Icon() { if (value) ::DestroyIcon(value); } };
    struct Request { std::wstring key; Resolver resolver; };
    struct Result { std::wstring key; std::wstring identity; std::shared_ptr<Icon> icon; };
    struct Entry { int index = 0; std::uint64_t used = 0; ULONGLONG validated = 0; std::wstring identity; };
    struct State {
        State(HWND target, UINT msg) : owner(target), message(msg), identity(++nextIdentity_) {}
        std::mutex mutex;
        HWND owner = nullptr;
        UINT message = 0;
        const std::uintptr_t identity;
        bool alive = true;
        bool running = false;
        std::deque<Request> queue;
        std::vector<Result> ready;
        std::unordered_set<std::wstring> pending;
    };
    void enqueue(const std::wstring& key, Resolver resolver) {
        std::lock_guard lock(state_->mutex);
        if (!state_->alive) return;
        if (!state_->pending.contains(key) && state_->pending.size() < kMaximumEntries) {
            state_->pending.insert(key);
            state_->queue.push_back({ key, std::move(resolver) });
        }
        if (state_->running || state_->queue.empty()) return;
        state_->running = true;
        try {
            std::thread([state = state_] { run(state); }).detach();
        } catch (...) {
            // Leave queued read-only work available for the next request; never
            // execute Shell I/O as a fallback on the painting thread.
            state_->running = false;
        }
    }
    static void run(const std::shared_ptr<State>& state) {
        for (;;) {
            Request request;
            {
                std::lock_guard lock(state->mutex);
                if (!state->alive || state->queue.empty()) { state->running = false; return; }
                request = std::move(state->queue.front());
                state->queue.pop_front();
            }
            Result result;
            result.key = std::move(request.key);
            try {
                Source source = request.resolver();
                result.identity = std::move(source.identity);
                auto icon = std::make_shared<Icon>();
                SHFILEINFOW info{};
                const bool generic = source.path.empty();
                if (::SHGetFileInfoW(generic ? L".exe" : source.path.c_str(),
                    generic ? FILE_ATTRIBUTE_NORMAL : 0, &info, sizeof(info),
                    SHGFI_ICON | SHGFI_SMALLICON | (generic ? SHGFI_USEFILEATTRIBUTES : 0)) && info.hIcon) {
                    icon->value = info.hIcon;
                    result.icon = std::move(icon);
                }
            } catch (...) { /* The generic icon is an explicit failure fallback. */ }
            {
                std::lock_guard lock(state->mutex);
                if (!state->alive) { state->running = false; return; }
                state->ready.push_back(std::move(result));
                ::PostMessageW(state->owner, state->message, static_cast<WPARAM>(state->identity), 0);
            }
        }
    }
    inline static std::atomic_uintptr_t nextIdentity_{0};
    HIMAGELIST images_ = nullptr;
    std::shared_ptr<State> state_;
    std::unordered_map<std::wstring, Entry> entries_;
    std::uint64_t useSerial_ = 0;
};

} // namespace Ksword::Ui
