# Colour Management

Bloom keeps the working image in a scene-linear colour space and applies display transforms only at
the Viewer and display-referred outputs. Set the project colour configuration before authoring a
show so colours have an unambiguous interpretation.

## Set up a show

Open File → Project Settings… → Colour, then:

1. Choose **Bloom Neutral v1** for the default Rec.709 scene-linear workflow, or choose **ACES 1.3
   CG (OCIO built-in)** for the ACES pipeline.
2. Choose the working colour space. Bloom preselects the configuration's `scene_linear` role. In
   the ACES CG configuration this is normally **ACEScg**; **ACES2065-1** is also available when a
   composition specifically needs it.
3. Review the read-only config name, revision digest, and display list before applying.

External `config.ocio` and `.ocioz` references remain explicit project dependencies. `$OCIO` may
suggest a file in the picker, but Bloom never uses the environment variable as an ambient runtime
fallback. The saved project contains the locator and content revision that were selected.

## Composition overrides

Properties → Composition → Working space is **Inherit** by default. Choose a colour space exposed by
the project's selected configuration to override it for that composition. A missing or non-scene-
linear colour space is refused; choosing a name that only exists in another configuration does not
silently switch configurations.

## Authored colours

Solid, Text, and Shape colour values are numeric in the effective working space. The colour picker
shows sRGB-encoded editing values and converts them through the selected OCIO configuration. Changing
the working space therefore re-interprets existing numeric values. Use the undoable **Convert
existing colours** command when preserving their previous appearance is required.

The Viewer and PNG output transform from the effective working space to the selected display/view.
Flat EXR keeps scene-linear pixels and writes the working-space id plus its primaries in the EXR
header. Projects that stay on `lin_rec709_scene` retain Bloom's earlier reference-display and output
goldens.
