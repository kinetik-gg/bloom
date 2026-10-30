# Color Management

Bloom keeps the working image in a scene-linear color space and applies display transforms only at
the Viewer and display-referred outputs. Set the project color configuration before authoring a
show so colors have an unambiguous interpretation.

## Set up a show

Open File → Project Settings… → Color, then:

1. Choose **Bloom Neutral v1** for the default Rec.709 scene-linear workflow, or choose **ACES 1.3
   CG (OCIO built-in)** for the ACES pipeline.
2. Choose the working color space. Bloom preselects the configuration's `scene_linear` role. In
   the ACES CG configuration this is normally **ACEScg**; **ACES2065-1** is also available when a
   composition specifically needs it.
3. Review the read-only config name, revision digest, and display list before applying.

External `config.ocio` and `.ocioz` references remain explicit project dependencies. `$OCIO` may
suggest a file in the picker, but Bloom never uses the environment variable as an ambient runtime
fallback. The saved project contains the locator and content revision that were selected.

## Composition overrides

Properties → Composition → Working space is **Inherit** by default. Choose a color space exposed by
the project's selected configuration to override it for that composition. A missing or non-scene-
linear color space is refused; choosing a name that only exists in another configuration does not
silently switch configurations.

## Authored colors

Solid, Text, and Shape color values are numeric in the effective working space. The color picker
shows sRGB-encoded editing values and converts them through the selected OCIO configuration. Changing
the working space therefore re-interprets existing numeric values. Use the undoable **Convert
existing colors** command when preserving their previous appearance is required.

The Viewer and PNG output transform from the effective working space to the selected display/view.
Flat EXR keeps scene-linear pixels and writes the working-space id plus its primaries in the EXR
header. Projects that stay on `lin_rec709_scene` retain Bloom's earlier reference-display and output
goldens.

## Review the show through the intended display

For an ACES CG review, open the Viewer footer's **Display / View** menu and choose **Rec.1886
Rec.709 - Display / ACES 1.0 - SDR Video**. The choice is remembered per Viewer and per config
revision; changing the project color configuration returns the menu to that config's default.
This is a review transform only: it changes Viewer and RAM Preview pixels, never the working-space
process image or export output.

If the composition contains a look-tagged OCIO File Transform, switch on the footer's **Look**
toggle to include it while reviewing, or switch it off to inspect the underlying grade. The toggle
is disabled when the composition has no look-tagged effect. Export always includes active look
effects; use an authored Bypass value when the exported handoff itself must omit one.

Inside the composition frame the Viewer shows the composition's **Background Color** (Composition
Settings) behind transparent pixels, as a display color: what you pick in the chip is what the
frame shows. Switch on **Transparency Grid** (the footer toggle or **View > Transparency Grid**) to
see a checkerboard there instead. **View > Background** only changes the canvas around the frame.
None of these reach export: exported alpha stays transparent. The Background Color is a Viewer
display setting only; renders, exports and captures keep transparency, as in After Effects. Its
field says so in a tooltip.

Move the pointer over the Viewer to read the probe in the window status bar. **W** reports the
un-premultiplied working-space RGBA authored by the process frame; **D** reports the display value
the Viewer paints. Ctrl-click the probe cell to switch D between 8-bit encoded values and float
values before display encoding. The tooltip names both the working-space id and selected display.

## Importing plates and camera footage

Assets use **Input color space: Auto** by default. Bloom resolves that choice from the selected
project configuration and shows the result in the Assets tooltip and the source row in Properties.
8/16-bit PNG, JPEG, and TIFF use the config's sRGB-texture space. EXR plates use their declared
chromaticities: AP0 maps to **ACES2065-1**, AP1 to **ACEScg**, and Rec.709/D65 to the config's
Rec.709 scene space. An EXR with no chromaticities assumes the working space and shows a warning;
unrecognized chromaticities require an explicit choice.

For camera footage, Bloom reads the stream's container color tags. Ordinary Rec.709 and Rec.601
footage (phone and camera H.264, for example) is shown as video players show it: Bloom decodes its
YCbCr with the stream's matrix and range and reads the result with the sRGB transfer, the same
sRGB-texture space it uses for 8-bit stills, so the footage matches a player and your sRGB images
without adjustment. sRGB-tagged footage uses the same space, and linear footage uses the working
space. To work with the camera's scene light instead, choose the Rec.709 camera space (the BT.709
inverse OETF; the picker currently lists it as **Rec.709 video**) as the input color space. The video asset tooltip retains the numeric container
tags. Footage with missing color tags is not refused: Bloom follows the usual NLE convention and
assumes Rec.709 for HD and larger pictures and Rec.601 for SD, with video (limited) range, and
fills only the tags that are missing. The asset then shows an Info mark and the note "Color tags
missing — assumed Rec.709 with the sRGB transfer. Change in Assets → Input Color"; choose an
explicit input color space to replace the assumption. A project saved by a Bloom version before
this default (document 1.28) shows a one-time note when you open it if any of its video looks
different now; saving the project records the new default. ARRI
LogC3 and RED Log3G10 are not reliable container tags, so choose their color space explicitly in
the searchable **Input color space** picker. The picker lists every non-data color space from the
selected configuration grouped by family.

Choose an explicit input id when the camera metadata is wrong or incomplete. The source-node choice
overrides the asset; **Auto** inherits the asset interpretation. Rec.2020, HLG, and PQ tags remain
unavailable on the unqualified preview path and report a typed diagnostic until a qualified config
mapping is explicitly selected. Changing the input choice or project working space re-decodes the
thumbnail, proxy, and rendered frame; no stale color conversion is reused.

## Set up a show LUT

1. Select the ACES 1.3 CG configuration and **ACEScg** working space.
2. Import the show's `.cube` file through the Assets panel. It appears with kind **LUT**.
3. In Nodes, choose **Add → Color → OCIO File Transform**. Connect the image producer to its
   **Input**, then connect **Image** to the next image node or Layer Output.
4. In Properties, select the LUT asset, set **Process Space** to **ACEScct**, and use the LUT
   author's interpolation and direction. The defaults are Linear and Forward.
5. Enable **Look** to mark this node as part of the show look. **Bypass** disables the node without
   changing its connections.

The File Transform performs ACEScg → ACEScct → LUT → ACEScct → ACEScg. It preserves alpha and keeps
imported media unchanged. An identity LUT is exactly equivalent to bypass. A missing, changed or
refused LUT leaves the image unchanged and reports why; use Assets → Replace Footage… to adopt an updated
file. LUT files are capped at 64 MiB with 3D edges no larger than 129. LUT import currently requires
Linux, and execution needs its qualified helper. Other targets preserve saved nodes and assets and
show an unavailable diagnostic.

**OCIO Color Space Transform** offers explicit **From** and **To** selectors from the project
config. Empty selectors mean the composition working space. For an explicit graph with CSTs on
either side of a LUT, set the LUT's Process Space to the working space so it processes the values
arriving at that point without adding another conversion. The surrounding CSTs then own the
ACEScg → ACEScct and ACEScct → ACEScg conversions.

The Look marker identifies the nodes that a look preview can skip. Use each node's **Bypass**
parameter to disable it. Exports include active effects; a preview-only bypass does not change
export. For a VFX handoff without the look, set the relevant nodes' **Bypass** parameters explicitly
before exporting, then restore them afterward.
