# UI grammar validation and captures

The UI-3 fixture renders the real MainWindow with Viewer, Nodes, Timeline, Assets and
Properties. It uses a CPU-ready preview, isolated settings, a selected Solid background with its timeline groups expanded and its Properties visible, plus a
text layer, with its node positions authored through MoveNodes. Focus and pointer hover are cleared before every capture.

| Rule | Enforcement |
| --- | --- |
| Kit vocabulary | C++ `ui_grammar` raw-control scan; kit control tests |
| Metric grid | Dimension scan; `ui_grammar_audit_tests.cpp` at DPR 1, 1.5 and 2 |
| Crisp icons | SVG QIcon engine; every icon and weight tested at DPR 1, 1.25, 1.5 and 2; whole-window physical-size audit |
| Declared chrome | `EditorChromeSpec`; one `EditorArea` builder and measured overflow; width-sweep and hosted chrome tests |
| Shared rows | `KRow` in Assets and timeline; `KPropertyRow` in Properties, node cards and timeline groups; geometry audit and window-level row command tests |
| Typography | Font-construction scan; Inter family-resolution and embedded SHA-256 tests |
| Canvas painting | Exact canvas-owner rules; empty allowlist enforced in the C++ scanner |
| Window appearance | Two full-window golden tests; tolerance and approval procedure in `src/ui/tests/goldens/README.md` |

The migration baseline contained **105** entries, of which GRAMMAR-1 left **95**.
GRAMMAR-2 removes all remaining entries: the allowlist now has **0** lines. A nonempty
allowlist itself fails the scanner; every live violation is reported. Bare default-constructed
controls and literal extent arguments following nested token expressions are covered.

The common metrics are Control 26, HeaderRow/FooterRow/ListRow 32, PropertyRow 28,
IconChrome 16, IconControl 20 and ToggleCell 24. Dropdown widths are 100, 108, 120 and a
240 maximum for expanded header selectors. Compact density changes width, not control height.
KMenuButton, KIconButton, KIconToggle, KLabel, KSearchField, KRow and KPropertyRow belong to
the kit, as does `makeMenu`. KPanelSwitcher is a compatibility alias of KDropdown, sharing its
model, popup and paint path. The icon variant retains the legacy default name.

Existing named controls retain their identities, including editorTypePicker, editorHeader,
editorFooter, maximizeAreaButton, the five panels' declared header/overflow/footer names,
viewer selectors and transport controls, layerBlendingDropdown, layerParentDropdown,
assetsTree/assetsSearchField and Properties fields. Footer hosting wraps the declared row so
its name survives. New timeline toggle cells are named timelineLayerToggle0 through 3.

Inter v4.1 Regular, Medium and SemiBold are unmodified static files. Archive and member hashes,
license and extraction members are recorded in `src/ui/kit/third_party/inter/manifest.json`
and `provenance.md`. Geist Mono remains the value face. The render module's DejaVu Sans Book
file and all fenced document/runtime/render/project/command modules are unchanged.

Normal golden runs write `build/ui3-window-dpr1.png` (1920 × 1200) and
`build/ui3-window-dpr15.png` (2880 × 1800). The references approve the status text row, complete sidebar, viewer work area and tools,
ordered footer, node-card rows and sockets, and shared empty timeline rows. The existing
menu bar is unchanged. The metric audit also checks status visibility/type, all six tools,
node control containment and socket alignment, and timeline menu fit at 1600 and 1920 pixels.
The [grammar](ui-grammar.md) lists the new kit classes, semantic tokens and object names.

Acceptance is recorded against the final commit in the external task progress log: native and
desktop builds and complete ctest suites, format/fix and format/check, quality checks, and the
rebuilt Qt 6.8.3 desktop suite with that installation's library/plugin paths. Re-running a gate
never updates references. A source fix requires repeating the exact-commit gates.

Qt qualification also binds menu-bar and table-header fonts during proxy polish. The fixture
installs that proxy in the application's order. This removes platform class-font overrides;
the original golden tolerance remains unchanged. Test entry points report fixture exceptions.

UI-3 adds `ui3_audit.hpp` to all three DPR metric runs: panel masks, shared chrome/row
insets, toggle/disclosure cells, label/control gutters, lane padding/divider/origin, grouped
timeline rows, accent ink, tool strip, measured dropdown minima, bounded menu columns,
category projection, empty-canvas search, single title bands, constant-width link activation,
and device-vector diamonds at two canvas zoom levels. Gesture tests target nested controls
through their row proxy and use the ruler's padded axis. New object names are enumerated in
ui-grammar.md. The document category enum stays unchanged under the task's ownership fence;
the UI projects the normalized categories and requires no persistence migration.
