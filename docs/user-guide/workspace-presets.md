# Workspace presets

A workspace preset is a named arrangement of panels. Pick one from the **Workspace** field on the
right of the toolbar.

| Preset | Arrangement |
| --- | --- |
| Default | Assets on the left, Viewer in the center, Properties on the right, and the Timeline across the bottom under Viewer and Assets |
| Node | Viewer on the left, Nodes on the right, Timeline across the bottom |
| Export | Assets on the left, Export on the right |
| Animation | Viewer and Properties over a tall Timeline |

Choosing a preset rearranges the panels. A panel the new arrangement keeps stays the same panel, so
its zoom, scroll position and selection are still there; a panel the arrangement has no place for
closes, and one it needs that is missing opens. Nothing in your project changes.

## Your own layouts

Arrange the panels the way you like, then choose **Save current layout as…** in the Workspace field
and name it. Your layouts are listed under the built-in ones. While one of them is chosen the field
also offers **Update ‘name’**, which stores the panels as they are now, and **Delete ‘name’…**,
which removes the layout (the panels on screen stay as they are).

When you change the arrangement after choosing a preset (drag a divider, split or close a panel,
change a panel's editor) the field reads **Default (edited)**, for instance. A new Bloom starts on
**Default**, and **Window → Reset Workspace** applies it again. The field reads **Custom** after you
delete the saved layout you were on.

Workspace presets belong to this computer's Bloom preferences, not to a project: they are the same
for every project you open.

## Opening the Export panel

Anything that opens the Export panel (File → Add to Export Queue, Show in Export in
the Jobs panel) follows three rules: an Export panel that is already open is brought forward and
focused, and no second one is made; otherwise the Timeline's panel turns into Export, in place; and
with no Timeline open either, a new Export panel opens along the bottom of the window.
