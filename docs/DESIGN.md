# Design

How the interface is built, why it looks the way it does, and what is still on
the list.

## Monochrome

The interface is grey. The only colour in it is information:

- the **waterfall**, whose colour is the measurement (and whose default ramp is
  now greyscale too, with four colour ramps one click away in Display)
- the **carrier marker**, which has to be findable in one glance against
  whatever colour the waterfall is showing
- the **S-meter's upper end**, where orange and amber mark the region above S9
- **state**: connected, warning, failed

Everything else - selection, focus, the primary button, every control - is
marked by brightness rather than by hue. On a grey field brightness is the
strongest signal available, it survives any form of colour blindness, and it
leaves the display as the only thing on screen with colour in it. An instrument
whose controls are as colourful as its readout teaches the eye to ignore both.

One consequence worth knowing about: the HUD over the waterfall keeps a fixed
light-on-dark palette rather than following the page theme, because the
waterfall is dark in both themes. In light mode the page foreground is
near-black, and a near-black readout over the waterfall is an invisible
readout. That was a real bug, found by looking at a screenshot.

## The system

Tokens live in `web/src/styles/tokens.css` and nothing else defines a colour, a
size or a duration. The vocabulary is shadcn/ui's - `background`/`foreground`
pairs, `muted` for recessive surfaces and text, `border`, `input`, `ring`,
`primary`, `destructive` - because it is the one most people working on a web
interface already know, and because naming a token for its *role* rather than
its shade is what makes a stylesheet legible a year later. `--radius` is
shadcn's 0.5rem.

Nothing is installed from those libraries. shadcn/ui, ReUI and beUI are React,
Tailwind and Motion; this client is Svelte without either of the others, and
its bundle is served off the receiver's own uplink to people on HF-grade
internet. What is taken is the
**anatomy**:

| Element | Anatomy borrowed from |
|---|---|
| Mode, filter, AGC selectors | shadcn/ReUI toggle group: a well, with a raised chip for the selection |
| Panel tabs | shadcn tabs: indicator on the strip's own border line, not a floating pill |
| Sliders | ReUI slider: 4px track, thumb with a ring on focus |
| Switch | shadcn switch: 40x24 track, thumb translating 16px |
| Band list, listener table | ReUI item and table: a row is a button; values right-aligned and tabular |
| Audio-gate button | beUI expanding arrow button, in six lines of CSS |

## Motion

The policy comes from the standards Emil Kowalski publishes as a skill
(`github.com/emilkowalski/skills`), read directly rather than paraphrased from
memory - which matters, because reading them turned up several things this
interface had wrong.

**The frequency table decides whether a thing animates at all.** Anything used
100+ times a day gets no animation ever; tens of times a day gets it reduced;
occasional things (dialogs, drawers) get a standard one. Tuning with the arrow
keys is the clearest case here, so the frequency readout has no transition at
all - measured at 0s.

**Easing.** `ease-out` for entering and exiting, `ease-in-out` for moving on
screen, and never `ease-in` on UI: it starts slow and delays the exact moment
the user is watching. The built-in CSS curves are too weak, so the tokens carry
the strong ones - `cubic-bezier(0.23, 1, 0.32, 1)` for UI and
`cubic-bezier(0.32, 0.72, 0, 1)`, the iOS drawer curve, for the sheet.

**Duration**, by element: press feedback 100-160ms, tooltips 125-200ms,
dropdowns 150-250ms, drawers 200-500ms. Nothing over 300ms except the drawer.

**Press feedback** was missing entirely and is the highest-value animation in
any interface: a control that does not acknowledge being pressed feels dead.
Every pressable element now sinks to `scale(0.97)` over 160ms - except the
frequency digits, which are dragged and nudged constantly and must stay
absolutely immediate.

**Physicality.** Nothing scales from zero; the dialog starts at 0.98. Popovers
grow out of their trigger rather than their own middle, so the tooltip sets its
`transform-origin` from the placement Floating UI actually resolved - which may
not be the one asked for, if it had to flip.

**Reduced motion means fewer and gentler, not none.** An earlier version zeroed
every duration, which also removed the fades that explain where a dialog came
from. Now the fades stay and only movement goes.

The earlier summary of this, from the article alone:

- **Every animation states its purpose or it does not exist.** There are three
  in the whole client: the dialog entrance, the sheet's snap, and the pulsing
  dot on "waiting for history" - which is there to say the receiver is working
  while the screen is still empty.
- **Nothing over 300ms.** Hovers are 100ms, entrances 220ms.
- **Nothing keyboard-initiated animates.** Tuning with the arrow keys fires
  hundreds of times a session; a readout that eased between values would make
  the receiver feel like it was thinking about it. The frequency digits have no
  transition at all, deliberately.
- `prefers-reduced-motion` zeroes both durations and stops every keyframe.

## Layout

The readout, the S-meter and the volume control are a HUD *over* the display,
not a row beneath it. The row they used to occupy cost 90px of waterfall on
every screen and left a band of mostly-empty surface across the bottom of a
phone. A real receiver puts them on the dial; so does this.

The controls are one tree for both layouts, with CSS deciding placement: a
sidebar on a wide screen, a draggable sheet on a phone. One tree means the
mobile layout cannot quietly rot into a second-class version of the desktop
one, which is how most web receivers end up painful on a phone.

## Accent discipline

There is one accent and it is spent on two things: what is selected, and where
the receiver is tuned. An earlier version filled every active segment - mode,
filter and AGC all at once - and the result had no focal point at all: nine
saturated chips competing, and the eye with nowhere to land. Selection is now a
raised chip. The only accent-filled button in the receiver is "Start audio",
because browsers will not start audio without a gesture and that button is the
one thing between a visitor and a working receiver.

## The waterfall

**The passband.** Its width stays true to frequency, even when narrower than
a pixel. An open yellow envelope at the ruler connects both cutoffs. While
dragging, a nearby readout shows the cutoff and bandwidth. Its short shoulders
are handles, not a measured
filter response; the exact cutoff passes through each shoulder's midpoint.
A guide aligns the selected edge with the waterfall while dragging; at rest
the waterfall has no artificial vertical lines. Narrow filters remain a single tuning
target until the edges are far enough apart to resize independently.

The tuning controls occupy their own row. They cannot cover the ruler when
the phone's control sheet expands. On a short display, the spectrum trace
and band-plan strip yield space to the passband, frequency scale and waterfall.

The open envelope follows the interaction convention of the
[classic WebSDR](http://websdr.ewi.utwente.nl:8901/). KiwiSDR and OpenWebRX were
also inspected, but the classic contour is the requested reference.
CW marks the received signal frequency; its oscillator
offset stays in the tuning model rather than moving that marker.

**Padding drawn as spectrum.** The level texture is 2048 columns wide and a
line is usually 1024 bins, so half of every row is zeros. The shader tested
"did this row cover this frequency?" in *texture* space rather than in *line*
space, so each row claimed data across twice the frequency range it actually
held. Pan left and the newly revealed strip blanked correctly, because the
coordinate went negative; pan right and it walked into the zero padding and
drew it as a noise floor - a dark band that read as signal-free spectrum
instead of as blank history. Rows now carry their real span and their column
count, and the coverage test happens in line space. Covered by a check in
`web/tools/browser-check.mjs`, which was first written panning the direction
that worked and passed against the bug; it now pans the other way.

**Auto-levels.** The floor sat 6 dB under the noise and the ceiling tracked the
strongest carrier, which put the noise floor in the bottom 7% of the ramp -
black, in every palette. The waterfall read as "nothing is being received" when
in fact everything was, and weak signals were invisible. The ceiling is now
capped relative to the noise rather than to the loudest thing in the band, so
the noise sits around 15% of the ramp where it has visible texture and the
40 dB above it gets most of the colour.

## Dependencies

Three, and each earns its place:

| Package | What it does here |
|---|---|
| `svelte` | the framework, the same as the admin panel's; its runes hold the state that updates a waterfall readout without re-rendering a tree |
| `@lucide/svelte` | icons, imported one at a time |
| `@floating-ui/dom` | tooltip placement that flips and shifts rather than sliding off a phone |

Svelte replaced Preact so that both pages are one framework. That cost the
listener page 14 kB gzipped of script: 63 kB where Preact's build was 49 kB.

`motion` and `clsx` were installed during the redesign and removed again: the
three animations that survived the motion policy are CSS keyframes, and class
composition never got complicated enough to need a helper. A dependency that is
only in `package.json` is still a dependency somebody has to audit.

## The admin panel

A separate page, `/admin`, that listeners never download. It is Svelte 5 with
Tailwind 4, bits-ui for the dialog, slider and switch behaviour (focus
trapping, keyboard, screen readers), svelte-sonner for notices and
`@lucide/svelte` icons: about 129 kB gzipped, loaded by one operator rather
than by everyone listening, which is why it can afford more than the receiver.

Its layout follows the operator's phone first. Settings are grouped rows with
the label on the left and the value on the right; a choice opens a sheet of
sensible steps instead of a slider over an arbitrary range. From laptop width a
page puts a narrow context column beside its settings (live figures, who is
listening, a preview, the facts the page cannot change) instead of stretching
the rows: a label and its value a monitor's width apart are hard to read
together. Colour is kept for meaning, as in the receiver: amber while something
is being retried, red when it needs the operator.

`web/tools/admin-check.mjs` holds it to that: it signs in, opens every page at
360, 390, 820, 1280 and 1920 pixels in both themes, and fails on console
errors, a page asking the receiver for the same thing in a loop, a page wider
than the phone, settings wider than 720 px, content under the phone's
navigation, unnamed controls, and on the main flows (change, see the save bar,
discard, complete a key in the configuration, filter the log) not working.


## Tooltips

`components/Tooltip.svelte`, positioned by Floating UI so it flips and shifts
rather than sliding off the edge of a phone. The `title` attribute used to do
this job badly: the browser decides when it appears, it cannot be styled, and
on a touch screen a long press gets the text-selection menu instead.

The delay rules are Kowalski's, and they are the reason it is worth the code: a
delay before the first tooltip so brushing past a row of controls does not fire
six of them, then **no delay and no animation while the group is warm**,
because once somebody is reading tooltips, waiting again for each one feels
broken.

The rule itself lives in `tooltip-timing.ts` as a pure module with unit tests,
because it is a rule and not a rendering concern. Measured in a real browser,
driving the pointer events from inside the page:

| | Delay | Entrance |
|---|---|---|
| First tooltip | 479 ms | animates |
| While the group is warm | **37 ms** | none |
| After the warm window expires | 467 ms | animates |

Worth recording how that was nearly mismeasured. Driving the same test through
Playwright's own `hover()` reported 906ms for the warm case and the group
apparently never warming - because `hover()` plus `waitForSelector` costs
several hundred milliseconds, which is longer than the 300ms warm window
itself. The harness was slower than the thing it was measuring.

## The registries, inspected

The `npx shadcn add` route does not work from this environment - it fetches
`ui.shadcn.com/r/registries.json`, which the egress proxy refuses - but the
registries are public git repositories, and those clone fine. So they were read
rather than guessed at.

**Rare UI** (`swamimalode07/rare-ui`), 16 components. Two are relevant to a
receiver and fourteen are not, which is the honest answer to "pick what fits":

- `family-drawer` - a bottom drawer that morphs between stacked views. The
  technique worth taking is that its crossfade duration *scales with how much
  the height changed*, clamped to 0.15-0.27s. Our control sheet switches
  between panels of very different heights and would benefit from the same.
- `bounce-sidebar` - a spring-animated active indicator for a vertical nav.
- The rest - a WebGL fluid orb, gravity letters, emoji reactions, a
  contribution heatmap, a notification bell - are for marketing pages. A
  ChatGPT-style voice orb in an SDR is exactly the thing this interface was
  being accused of.

Every component in these registries is React, Tailwind and Motion. Installing
them would mean adding all three to a client that ships in 63 kB over the
receiver's own uplink, so what is taken is anatomy and technique, written
natively.

## Settled by testing

Two items were on this list until a receiver with eight bands was actually put
in front of a phone:

- **The band strip stays a strip.** Eight bands read well in the top bar, the
  way browser tabs do; a vertical list would have cost the controls their room
  for no gain. What testing *did* find was a real break - the strip was
  pushing the whole page wider than the screen, because a flex child that
  cannot shrink below its content does not scroll, it stretches. It now
  shrinks, scrolls, and fades at its trailing edge so it is visible that there
  is more.

- **The waterfall was never broken.** It looked empty because of the
  auto-levels bug above, and because history genuinely takes half a minute to
  reach the bottom of a tall screen.

## Open items

- Saving a configuration from the admin panel still requires a restart to
  apply. Bands can be restarted individually from the panel, which covers the
  common case (a source that has gone away); a full reload without dropping
  listeners is the harder version and is not done.
