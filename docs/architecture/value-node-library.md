# Value Node Library

The value graph's own node catalogue. `layer-graph-model.md` owns the value graph's SHAPE -- two
graphs in one node set, socket kinds and promotion, drivers, reroutes, the fallback philosophy --
and this document owns the LIBRARY: what each node is, what it accepts, and what it does at the
edges.

It exists because the catalogue outgrew the section that held it. A table of eighty-odd rows inside
`layer-graph-model.md` would bury the model it belongs to.

## The Safe Parse Contract

Every node that reads a value OUT of text obeys this contract, and no node may obey a variant of it.

**A parsing node never fails.** It has three parts, and all three are mandatory:

| Part | Rule |
| --- | --- |
| `value` output | The parsed value when the text parses, the `fallback` operand's value when it does not |
| `valid` output | A Boolean: `true` exactly when the text parsed |
| `fallback` operand | Of the same kind as `value`, so a graph always has a defined number downstream |

There is no third state. Evaluation does not fail, no exception is thrown, no diagnostic is raised
for text a node simply does not accept -- unparsable text is an ordinary answer with `valid` false,
not an error. The only failure a library node can report is a MALFORMED PLAN: an operand that
arrived as the wrong kind, which the document's own typing already refuses.

**Parsing is locale-neutral.** `.` is the decimal separator; ASCII `0`-`9` are the only digits, so a
non-ASCII digit is INVALID rather than silently folded; there are no thousands separators unless a
node documents an explicit option for them (none does today). Surrounding ASCII whitespace is
trimmed first. A project that parses `1.5` reads 1.5 on every machine that opens it.

**The whole string is consumed.** `std::stod("12abc")` answers 12 and reports the rubbish through an
out parameter callers drop; here, text that is not entirely a number is not a number. There is no
`std::stod` anywhere in the library, and no conversion without a full-consumption check.

**Nothing is undefined.** Empty text, whitespace-only text, an overflowing magnitude, a digit a
radix does not have -- each is refused, not half-read. `inf`, `infinity` and `nan` are NOT accepted
in any spelling: they name a non-finite answer, and a value graph that silently acquired one would
poison every operand downstream of it. Hexadecimal float literals are not accepted either.

The one implementation lives in `bloom/core/safe_parse.hpp`, so the accepted forms are decided once
rather than per node.

### Accepted forms

| Reader | Grammar, after trimming |
| --- | --- |
| Scalar | `[+-]? ( digits ( '.' digits? )? \| '.' digits ) ( [eE] [+-]? digits )?` |
| Integer | `[+-]? digit+` in the node's radix; digits are `0`-`9` then `a`-`z`/`A`-`Z` for 10-35. No `0x`/`0b` prefix -- the radix is chosen on the node, and a prefix would be a second, contradictory way to say it |
| Boolean | `true`, `false`, `yes`, `no`, `on`, `off`, `1`, `0`, case-insensitive over ASCII |
| Color | `#RRGGBB` or `#RRGGBBAA`, `#` required, hexadecimal digits in either case. `#RGB` shorthand is NOT accepted: its expansion is a second contract |

A magnitude outside binary64's representable range is refused in BOTH directions: `1e999999` would
answer an infinity and `1e-400` would answer a zero the author did not write. A subnormal is inside
the range and parses.

`-0` parses, and answers negative zero: it is a representable finite double and the sign is
information the author typed. The most negative `std::int64_t` parses exactly; one past it is
refused rather than wrapped.

## The Formatting Contract

Formatting is the mirror image, and is EXPLICIT: precision, padding, prefix and suffix are operands
with defaults, never implied.

- Output is deterministic and independent of the process locale. `.` is the decimal separator
  whatever `LC_NUMERIC` says.
- `decimals` and `padWidth` are CLAMPED into their supported ranges rather than refused: a formatter
  has no failure an artist could act on, and a clamped precision still prints the number they asked
  about.
- Zero padding applies to the number INCLUDING its sign, the way `printf`'s `%0*d` does: the sign
  stays leftmost and the zeros follow it. `-7` at width 4 is `-007`.
- Trimming trailing zeros removes the fractional zeros and then a bare trailing point, and keeps the
  SIGN: `-0.004` at two decimals prints `-0`.
- A non-finite value has no decimal spelling this contract admits, so it formats as the EMPTY
  STRING -- the same answer the parser gives for text that is not a number, in the other direction.
  A prefix and suffix the artist authored still appear around it.

## How The Library Is Built

Every node below shares ONE lowering, `NodeLoweringKind::ValueUtility`, and one compiled kernel,
`CompiledValueUtility`. Its shape is DATA: `document::valueUtilityDescriptors()` names each type's
sockets, its inline selectors and its outputs, and four consumers read that one table -- the
registered `NodeDefinition`, the shape validation that checks a registered definition against it,
the compiler's lowering, and the evaluator's kernel. A node type therefore cannot be added half-way:
a descriptor with no kernel does not compile, and a kernel with no descriptor has nothing to lower.

The kernel enumeration is NOT persisted. A document stores the TYPE ID, and the type id is what
names the operation.

Every socket-backed operand is linkable and backed by a parameter of the matching kind, exactly as
the first slice's operands are. An inline SELECTOR -- a rounding mode, a radix -- has no socket, for
the reason the existing Math operation has none: it decides which computation the plan performs, so
it is known when the plan is built rather than delivered per frame.

## The Catalogue At A Glance

Sixty-two node types, in five groups. Each group has its own section below with the full shape and
the edge behaviour of every row.

| Group | Count | Category | What it is for |
| --- | --- | --- | --- |
| [Conversions](#conversions) | 20 | `Convert` | Every primitive kind to and from `String`, between the numeric kinds, and between `Color`, `Vector2` and `Vector3` |
| [Time conversions](#time-conversions) | 4 | `Time` | Seconds, frames and non-drop timecode, at the composition's own rate |
| [String utilities](#string-utilities) | 15 | `String` | Building, measuring, cutting and comparing text |
| [Math](#math) | 16 | `Math`, `Vector`, `Color` | The numeric gaps, plus the nine existing arithmetic nodes that moved into the section |
| [Logic](#logic) | 3 | `Logic` | Boolean combination and a range predicate |
| [Readouts](#readouts) | 4 | `Time` (frame), `Values` (composition) | What the composition and the frame are, as numbers |

Every node's socket names, its inline selectors and its outputs come from ONE record --
`document::valueUtilityDescriptors()` -- so the tables below describe the same table the registry,
the compiler and the evaluator read.

### What this slice did NOT change

- **No identity or semantics version moved.** These are new node TYPES only. The compiled plan, the
  evaluator, the image primitives, the scalar primitives and the animation sampler all keep their
  versions, and no golden pixel or output digest shifted.
- **No persistence step.** A new node type is not a stored-shape change. `NodeCategory` is not
  persisted either, so moving the arithmetic into `Math` needed no migration.
- **One UI change, and only one.** The Add search popup and the Add Node menu gained a **Math**
  section header, and the section order became Sources, Layers, Compositing, Values, Math,
  Utilities, Output. Node cards render every new node through the registry-driven generic path that
  already existed (`nodeOperandSelector`, `nodeOperandEditor`, `nodeOperandToggle`,
  `nodeOperandTextEditor`, `nodeOperandColorChip`); NO new `objectName` was introduced and none was
  renamed.

## Conversions

Category `Utilities`. Every row's inputs are listed in socket order; a **selector** is inline and
carries no socket.

| Node | Inputs | Outputs | Notes |
| --- | --- | --- | --- |
| Scalar To String | `value` Scalar, `decimals` Integer (2), `trimZeros` Boolean (false), `padWidth` Integer (0), `prefix` String, `suffix` String | `result` String | Fixed-point, then optional zero-trim, then zero-pad, then decoration. Non-finite prints as the empty number |
| Integer To String | `value` Integer, `padWidth` Integer (0), `prefix` String, `suffix` String; selector `radix` (10) | `result` String | Radix 2/8/10/16 offered; any radix 2-36 the document carries is honoured. Digits above 9 are lowercase |
| Boolean To String | `value` Boolean, `trueText` String (`true`), `falseText` String (`false`) | `result` String | The labels are the artist's; nothing is capitalized for them |
| Color To String | `color` Color, `includeAlpha` Boolean (false), `uppercase` Boolean (false) | `result` String | `#RRGGBB` or `#RRGGBBAA`. Channels are clamped into [0, 1] and rounded to nearest, ties away from zero |
| Vector 2 To String | `vector` Vector2, `separator` String (`, `), `decimals` Integer (2) | `result` String | Components joined with the authored separator, each fixed-point |
| Vector 3 To String | `vector` Vector3, `separator` String (`, `), `decimals` Integer (2) | `result` String | As above, across three |
| String To Scalar | `text` String, `fallback` Scalar | `value` Scalar, `valid` Boolean | Safe parse |
| String To Integer | `text` String, `fallback` Integer; selector `radix` (10) | `value` Integer, `valid` Boolean | Safe parse at the node's radix |
| String To Boolean | `text` String, `fallback` Boolean | `value` Boolean, `valid` Boolean | Safe parse |
| String To Color | `text` String, `fallback` Color | `value` Color, `valid` Boolean | Safe parse |
| Scalar To Integer | `value` Scalar; selector `mode` (Round) | `result` Integer | Round (ties away from zero), Floor, Ceiling, Truncate. NaN answers 0; a magnitude past the signed range SATURATES rather than wrapping |
| Integer To Scalar | `value` Integer | `result` Scalar | Exact below 2^53; the nearest double beyond. The same widening the Integer-to-Scalar promotion performs |
| Boolean To Scalar | `value` Boolean | `result` Scalar | `false` is 0, `true` is 1 |
| Boolean To Integer | `value` Boolean | `result` Integer | The same mapping |
| Scalar To Boolean | `value` Scalar | `result` Boolean | Nonzero is true. NaN is FALSE and negative zero is FALSE -- `value != 0` would call a number that is not a number set |
| Integer To Boolean | `value` Integer | `result` Boolean | Nonzero is true |
| Color To Vector 3 | `color` Color | `result` Vector3 | RGB only. Alpha has nowhere to go in a Vector 3; Separate RGBA is how a graph reads alpha as a number |
| Vector 3 To Color | `vector` Vector3, `alpha` Scalar (1) | `result` Color | Straight authoring values, no gamut or OCIO step implied |
| Vector 2 To Vector 3 | `vector` Vector2, `z` Scalar (0) | `result` Vector3 | Supplies Z |
| Vector 3 To Vector 2 | `vector` Vector3 | `result` Vector2 | DROPS Z. Not a projection: a perspective divide is a different operation with a camera behind it |

## Time Conversions

Category `Utilities`. All four read the COMPOSITION's frame rate -- the same rate a `Time` node's
`frame` output is computed from, so the two can never disagree about which frame an instant falls
in.

| Node | Inputs | Outputs | Notes |
| --- | --- | --- | --- |
| Seconds To Frames | `seconds` Scalar | `result` Integer | `floor(seconds * rate)`. Floored, not rounded: the frame an instant falls INSIDE is the frame being rendered. NaN answers 0; an unreachable magnitude saturates |
| Frames To Seconds | `frames` Integer | `result` Scalar | `frames / rate`, exact at a frame-aligned time |
| Seconds To Timecode | `seconds` Scalar | `result` String | Non-drop `HH:MM:SS:FF` |
| Timecode To Seconds | `text` String, `fallback` Scalar | `value` Scalar, `valid` Boolean | Safe parse; accepts `HH:MM:SS:FF`, `MM:SS:FF` and `SS:FF` |

### Drop-frame timecode is NOT supported

Bloom's timecode is NON-DROP, and that is documented rather than approximated. A drop-frame count is
a different mapping from frame numbers to wall clock -- it skips two labels a minute to keep a 29.97
count near real time -- and a node that printed `HH:MM:SS:FF` while meaning drop-frame would name a
different frame than the one it showed.

The frame field therefore counts to the rate's NOMINAL whole-number frame count: 24 at 24, 25 at 25,
30 at 30000/1001. At a fractional rate the label drifts from wall clock, which is exactly what
non-drop timecode does -- one wall-clock second at 29.97 is `00:00:00:29`, not `00:00:01:00`.

`;` in place of the last separator is the conventional spelling OF drop-frame, so `00:00:02;12` is
refused rather than read as non-drop.

### Timecode reading rules

- Fields are read from the RIGHT, so the last is always frames. `02:12` is `SS:FF`, `01:30:00` is
  `MM:SS:FF`.
- Each field is one or more ASCII digits and nothing else: no sign inside a field, no spaces around
  the separators.
- An optional `+` or `-` may lead the WHOLE label. A negative time prints one leading `-` rather
  than a sign on one field.
- A field past its own modulus names no instant and is refused: minutes and seconds at 60 or more,
  a frame field at or past the nominal count.
- Hours are NOT wrapped at 24 and not truncated to two digits. A composition may legitimately be
  longer than a day, and a label that silently rolled over would name the wrong instant.

## String Utilities

Category `Utilities`.

| Node | Inputs | Outputs | Notes |
| --- | --- | --- | --- |
| Concatenate | `a`, `b`, `c`, `d` String, `separator` String | `result` String | EMPTY parts are skipped, separator and all -- four operands is the shape, but most uses fill two |
| Format | `pattern` String (`{0} {1}`), `a`, `b`, `c`, `d` String | `result` String | `{0}`-`{3}`; `{{` and `}}` escape a brace. A slot the node does not have becomes NOTHING; an unmatched brace is literal |
| Length | `text` String | `result` Integer | Unicode scalars, not bytes |
| Substring | `text` String, `start` Integer (0), `length` Integer (-1) | `result` String | Clamped at both ends. A NEGATIVE length means "to the end", which is what makes a fresh node answer the whole string |
| Character At | `text` String, `index` Integer (0), `fallback` String | `value` String, `valid` Boolean | One scalar, or the fallback |
| Split | `text` String, `separator` String (`,`), `index` Integer (0), `fallback` String | `value` String, `valid` Boolean | An EMPTY separator splits nothing and answers invalid. An empty PIECE is a real answer, which is why the fallback and the empty string must not look alike |
| Replace | `text` String, `search` String, `replacement` String | `result` String | Every occurrence, left to right, non-overlapping. An empty search answers the text unchanged |
| Trim | `text` String | `result` String | ASCII whitespace, both ends |
| Case | `text` String; selector `mode` (Upper) | `result` String | Upper, Lower, Title. ASCII only |
| Pad | `text` String, `width` Integer (0), `fill` String (space); selector `side` (Start) | `result` String | Width in scalars. The fill contributes its FIRST scalar only; an empty fill pads nothing |
| Repeat | `text` String, `count` Integer (1) | `result` String | Count clamped into [0, 1024] |
| Contains | `text` String, `search` String, `caseSensitive` Boolean (true) | `result` Boolean | An empty search is contained |
| Starts With | `text` String, `search` String, `caseSensitive` Boolean (true) | `result` Boolean | An empty search matches |
| Ends With | `text` String, `search` String, `caseSensitive` Boolean (true) | `result` Boolean | An empty search matches |
| String Equals | `a` String, `b` String, `caseSensitive` Boolean (true) | `result` Boolean | Byte equality, or ASCII-folded equality |

### Text is measured in Unicode scalars

`Length`, `Substring`, `Character At` and `Pad` count what an artist counts: `é` is one character
whether it arrived as two bytes or one, and an emoji is one character rather than four.

A byte that is not a well-formed UTF-8 scalar counts as ONE unit and survives unchanged. That is the
only total answer: a document may carry text from anywhere, and refusing to measure a string because
one byte is malformed would stop a whole graph at a caption nobody can see is broken.
`core::decodeUtf8Scalar()` decides what "well formed" means, so the library has no second opinion
about UTF-8.

### Case mapping is ASCII-only, deliberately

`Case` and every case-insensitive comparison fold ASCII and pass everything else through unchanged.
A Unicode case mapping is locale-sensitive (Turkish dotless i), context-sensitive (Greek final
sigma) and tied to a Unicode table version, so a node claiming to do it would answer differently on
a different machine or in a different year. That is documented rather than silently approximated.

Title case upper-cases the first ASCII letter or digit of each run and lower-cases the rest, so
`two-part name` becomes `Two-Part Name`.

### Nodes that can be asked for something that is not there

`Character At` and `Split` are not parsers, but an index can name a piece that does not exist. They
answer with the SAME shape the safe parse contract defines -- a value, a `valid` Boolean and a
`fallback` operand -- so a graph has one rule to learn rather than two.

## Math

Category `Math`. The section holds the ARITHMETIC and nothing else: the existing Math, Vector Math,
Vector Measure, Map Range, Clamp, Mix, Mix Color, Compare and Random nodes moved into it from
`Utilities`, and every numeric node below was added to it. Switch, Separate/Combine, Reroute, the
conversions and the logic stayed in `Utilities`, because a predicate and a wire are not arithmetic.

| Node | Inputs | Outputs | Notes |
| --- | --- | --- | --- |
| Integer Math | `a` Integer, `b` Integer; selector `operation` (Add) | `result` Integer | Add, Subtract, Multiply, Divide, Modulo, Minimum, Maximum |
| Rounding | `value` Scalar; selector `mode` (Round) | `result` Scalar | Round, Floor, Ceiling, Truncate |
| Sign | `value` Scalar | `result` Scalar | -1, 0 or 1 |
| Wrap | `value` Scalar, `min` Scalar (0), `max` Scalar (1) | `result` Scalar | Folded into the HALF-OPEN interval, so a value at the maximum is the minimum one period along |
| Snap | `value` Scalar, `step` Scalar (1) | `result` Scalar | Nearest multiple. A zero or non-finite step is no grid, so the value passes through |
| Ping-pong | `value` Scalar, `length` Scalar (1) | `result` Scalar | A triangle wave over [0, length]. A non-positive length has no period and answers 0 |
| Smoothstep | `edge0` Scalar (0), `edge1` Scalar (1), `value` Scalar | `result` Scalar | The frozen primitive's own operand order. A degenerate edge pair is outside its domain and answers 0 |
| Degrees To Radians | `degrees` Scalar | `result` Scalar | |
| Radians To Degrees | `radians` Scalar | `result` Scalar | |
| Rotate 2D | `vector` Vector2, `degrees` Scalar, `pivot` Vector2 | `result` Vector2 | Counter-clockwise in a Y-up frame, about the authored pivot |
| Polar To Cartesian | `radius` Scalar (1), `degrees` Scalar | `result` Vector2 | |
| Cartesian To Polar | `vector` Vector2 | `radius` Scalar, `angle` Scalar | Angle in degrees over (-180, 180]. The ORIGIN answers 0, because it has no direction |
| Separate HSV | `color` Color | `hue`, `saturation`, `value`, `alpha` Scalars | Hue in degrees over [0, 360). A grey has no hue and answers 0 |
| Combine HSV | `hue`, `saturation`, `value` (1), `alpha` (1) Scalars | `result` Color | Hue WRAPS; saturation is clamped into [0, 1] |
| Hue Shift | `color` Color, `degrees` Scalar | `result` Color | Hue is periodic, so the shift wraps rather than clamping |
| Luminance | `color` Color | `result` Scalar | Rec.709 weights through `bloom/core/color.hpp`, never retyped in a kernel. Alpha takes no part |

### One rounding rule

`Rounding`, `Snap`, `Scalar To Integer` and the existing `Math` node's rounding operations all route
through `core::primitives::evaluateScalar()`. That matters at a TIE: the frozen primitive uses
`std::nearbyint`, which rounds half to EVEN, so 2.5 is 2 and 3.5 is 4. Calling `std::round` in one
node and the primitive in another would make two nodes an artist reads as the same operation
disagree about exactly the value they are most likely to type.

### Integer arithmetic saturates

Signed overflow is undefined in C++ and a value graph's operands come from anywhere, so every
`Integer Math` operation is bounded: adding one to the largest integer answers the largest integer
rather than the smallest. Divide and Modulo by zero answer 0 -- the documented fallback the scalar
tranche already gives -- and the one signed division that overflows (the most negative value over
-1) saturates too. Modulo is truncated division's remainder, so it takes the sign of the DIVIDEND,
which is C's rule and the one every language an artist is likely to have met uses for `%`.

### Vector length, normalize, distance and dot are already here

They were checked before being added and are NOT duplicated: `Vector 2/3 Measure` already offers
Length, Dot Product and Distance, and `Vector 2/3 Math` already offers Normalize. A second spelling
of each would be two nodes an artist has to choose between for one operation.

## Logic

Category `Utilities`. A predicate is not arithmetic, so these did not move into `Math`.

| Node | Inputs | Outputs | Notes |
| --- | --- | --- | --- |
| Boolean Logic | `a` Boolean, `b` Boolean; selector `operation` (And) | `result` Boolean | And, Or, Xor, Nand, Nor |
| Boolean Not | `value` Boolean | `result` Boolean | |
| In Range | `value` Scalar, `min` Scalar (0), `max` Scalar (1) | `result` Boolean | INCLUSIVE at both ends. A reversed pair names no interval, so nothing is inside it; NaN is in no interval |

## Readouts

Categories `Time` for frame readouts and `Values` for composition readouts, because a readout is where a value comes
FROM. None has an input or a parameter at all: its value belongs to the composition or to the
evaluation request, not to the document's authored values.

| Node | Inputs | Outputs | Notes |
| --- | --- | --- | --- |
| Frame Number | none | `result` Integer | The frame the request time falls in, from the same exact rational arithmetic a `Time` node's `frame` output uses |
| Frame Rate | none | `result` Scalar | The composition's frame rate as a number |
| Composition Duration | none | `result` Scalar | Seconds, so it compares directly with a `Time` node's `seconds` output |
| Composition Size | none | `result` Vector2 | The composition's pixel extent |

### Three of these are constants in the plan

`Frame Rate`, `Composition Duration` and `Composition Size` describe the COMPOSITION, and a plan is
compiled from one document snapshot. Changing a composition's format or duration is a document edit
that recompiles the plan, so the compiler bakes each of them into a `CompiledValuePassthrough`
holding the value it read. They therefore cost nothing per frame, are never time-dependent, and
cannot disagree with the settings they came from.

`Frame Number` is the one that describes the FRAME, so it is the one kernel the evaluator computes,
and the one readout the plan's time-dependence analysis marks as time-dependent -- exactly as it
marks a `Time` node.

### Layer Bounds is NOT delivered

Task UTIL-1 asked for a `Layer Bounds` readout -- an Image input answering size, origin and anchor
from the compiled plan's evaluated bounds -- "if it needs plan plumbing beyond a day's work, deliver
the others and disclose". It does, and this is the disclosure.

The obstruction is an ORDERING one, not an amount of typing. The value graph is compiled and
evaluated in its own pass BEFORE any image operation runs, which is what lets an image operation
read a value-graph output as a parameter. Evaluated bounds are produced BY the image pass. A value
node that read them would need the image pass's results to exist before the value pass runs, which
inverts the one ordering the two-pass design rests on, and a composition could then contain a cycle
the existing acyclic check cannot see -- a Layer Bounds feeding a transform whose own bounds it
reads.

Delivering it properly means either a second value pass that runs after the image pass (and a rule
about which parameters may read it), or per-node bounds resolution hoisted out of evaluation into
compilation. Both are design changes to the evaluation model rather than another node, so the other
four readouts ship and this one does not.

### Why these are nodes rather than implicit coercions

The connect-time promotion whitelist (`layer-graph-model.md`, **Socket Kinds And Promotion**) admits
exactly five widenings, each with one answer and no lost information. Every conversion above is
outside that set: it either loses information (Vector 3 to Vector 2), invents a spelling (anything
to String), or can fail (anything from String). A refusal the artist can see, followed by a node
they placed deliberately, is better than a silent coercion whose rule they have to remember.

The current UI category order and normalized family mapping are owned by [UI grammar](../ux/ui-grammar.md).
This presentation projection leaves the non-persisted document category enum unchanged.
