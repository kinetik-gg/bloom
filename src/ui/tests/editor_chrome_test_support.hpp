#pragma once
#include <bloom/ui/editor_area.hpp>
namespace bloom::ui::test {
inline QWidget* header(EditorChromeProvider& editor) {
    auto& spec = editor.editorChrome();
    if (spec.hosted)
        spec.hosted();
    return EditorArea::buildChromeRow(spec.header, nullptr);
}
inline QWidget* footer(EditorChromeProvider& editor) {
    auto& spec = editor.editorChrome();
    if (spec.hosted)
        spec.hosted();
    return EditorArea::buildChromeRow(spec.footer, nullptr, true);
}
} // namespace bloom::ui::test
