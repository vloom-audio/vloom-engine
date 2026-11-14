// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#pragma once

#include <vloom/engine/plugin/AudioProcessor.hpp>

#include <cstdint>
#include <vector>

namespace vloom::test {

/// Records what a processor tells the host about its editor.
struct EditorRecorder final : EditorListener {
    struct Edit {
        std::uint32_t id = 0;
        double value = 0.0;
    };

    void editorResizeRequested(EditorSize size) override { resizes.push_back(size); }
    void editorClosed() override { ++closed; }
    void parameterEdited(std::uint32_t id, double value) override { edits.push_back({id, value}); }

    std::vector<EditorSize> resizes;
    std::vector<Edit> edits;
    int closed = 0;
};

/// A stand-in for a native window: the test plugins only check it is set.
inline void* fakeWindow() noexcept
{
    static int window = 0;
    return &window;
}

} // namespace vloom::test
