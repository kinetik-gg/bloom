# UI grammar validation and captures

The GRAMMAR-1 fixture renders the real MainWindow with Viewer, Nodes, Timeline, Assets and
Properties. It uses a CPU-ready preview, isolated settings, a solid background and a selected
text layer. Focus and pointer hover are cleared before every capture.

| Rule | Enforcement |
| --- | --- |
| Kit vocabulary | C++ `ui_grammar` raw-control scan; kit control tests |
| Metric grid | Dimension scan; `ui_grammar_audit_tests.cpp` at DPR 1, 1.5 and 2 |
| Crisp icons | SVG QIcon engine; every icon and weight tested at DPR 1, 1.25, 1.5 and 2; whole-window physical-size audit |
| Declared chrome | `EditorChromeSpec`; one `EditorArea` builder and measured overflow; width-sweep and hosted chrome tests |
| Shared rows | `KRow` in Assets and timeline; `KPropertyRow` in Properties; geometry audit and window-level row command tests |
| Typography | Font-construction scan; Inter family-resolution and embedded SHA-256 tests |
| Canvas painting | Exact canvas-owner rules and explicit migration allowlist in the C++ scanner |
| Window appearance | Two full-window golden tests; tolerance and approval procedure in `src/ui/tests/goldens/README.md` |

The migration baseline contained **105** file/line/kind entries; **95** remain for GRAMMAR-2.
Entries were relocated by their original source text after line changes, and removed only when
the violation disappeared. No new debt was added. These are deliberate migration exceptions,
including older dialog controls and non-canvas painters; they do not describe the finished
contract for new UI code.

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

Normal golden runs write `build/grammar1-window-dpr1.png` (1920 × 1200) and
`build/grammar1-window-dpr15.png` (2880 × 1800). The references deliberately approve Inter,
shared chrome, real timeline toggles, fixed Assets columns and the complete Properties row
pitch. Narrow property rows remove column gaps while preserving field and label floors.

Acceptance is recorded against the final commit in the external task progress log: native and
desktop builds and complete ctest suites, format/fix and format/check, quality checks, and the
rebuilt Qt 6.8.3 desktop suite with that installation's library/plugin paths. Re-running a gate
never updates references. A source fix requires repeating the exact-commit gates.

Qt qualification also binds menu-bar and table-header fonts during proxy polish. The fixture
installs that proxy in the application's order. This removes platform class-font overrides;
the original golden tolerance remains unchanged. Test entry points report fixture exceptions.
