// Deterministic scripted input for automated menu tests (M4).
//
// --kuf_input_script="<tick> <verb> [args]; ..." applies events at a game tick (1-based; the frame numbers of earlier
// versions: one frame per tick). Script time follows the game, not the presentation (K38): an input item at N is
// applied on the game thread when tick N + 1 starts, before that tick reads input, so exactly tick N + 1 sees it
// whatever the frame rate and however far the game thread runs ahead of the render thread; capture, trace and freeze
// run after tick frame N is presented (the frames showing a tick for the first time; the first comes before the first
// tick, so tick frame N shows tick N - 1). Events go through the same path as real input: keyboard and mouse
// events into KufInputDriver/KeyboardPad, controller events into an SDL virtual gamepad that the SDK's SDL driver sees
// like a plugged-in controller (its joysticks updated at that tick start). The `sdl` events go through the window's
// event queue and so reach the game when the UI thread handles them, not at a tick boundary.
//
//   down <Key> | up <Key>       key or mouse button (key names as in docs/INPUT.md)
//   tap <Key>                   down and up at the same instant (exercises the short-tap latch)
//   press <Key>                 down now, up 4 ticks later
//   wheel <detents>             mouse wheel (+ = up)
//   mouse <x> <y> <w> <h>       pointer at window pixel (x, y) in a w x h window
//   focus on|off                window focus gained / lost
//   pad connect|disconnect      attach / detach an SDL virtual Xbox-layout gamepad
//   pad down|up <button>        a b x y back guide start lstick rstick lb rb up down left right
//   pad press <button>          down now, up 4 ticks later
//   pad axis <axis> <value>     leftx lefty rightx righty (SDL convention: +y = down) lt rt, -32768..32767
//   sdl key down|up|repeat <K>  a real SDL key event (SDL key names: Return, Escape, Down, F1, ...) pushed into
//                               the window's event queue: exercises SDL -> window -> key translation -> driver
//   sdl button down|up <b> <x> <y>, sdl click <b> <x> <y>   real mouse button events (left|right|middle)
//   sdl wheel <detents> <x> <y> | sdl motion <x> <y>          real wheel / pointer motion events
//   sdl focus on|off            real window focus events
//   sdl resize <w> <h>          resizes the (offscreen) window through SDL on the UI thread
//   pointer                     logs the driver's pointer position in guest pixels
//   look <dx> <dy>              mouse-look movement in pixels, as from the captured pointer (gameplay only)
//   capture <label>             captures the frame showing that tick, recorded with the label (capture_check rules)
//   freeze <seconds> <label>    M6D diagnostic (decoupled pacing only): holds the game while the same tick is drawn
//                               again and again, captures three of those frames and reports the memory it changed
//   trace                       GPU-traces the next frame showing a new tick (SDK patch 0009's trace, which must be
//                               on: --gpu_trace_frame=2000000000), so a trace marks a game moment (K35)
//   log <text>                  marks the log
// Instead of a tick number, an item may start with an anchor, @<context>[#n]+<ticks>: that many ticks after the tick
// in which the game entered the context for the n-th time (menu, prompt, gameplay; n defaults to 1). Loading and
// tutorial timing vary between runs, so gameplay scenarios anchor to the game's own state.
// "<when> repeat <ticks> until <context>[#n] <verb> [args]" repeats a tap/press/down/up/pad/sdl item every
// <ticks> ticks until the game has entered <context> n times (e.g. skip a cutscene until play starts); a pending
// release (a press's up, a key or pad button up, an axis back to 0) still applies once after that, so a repeat never
// leaves an input held (K42).
// Every applied event is logged as "[kuf-input-script] tick N: ...". Malformed scripts fail at start.
// --kuf_input_script_file=<path> reads the same items from a file instead: one or more per line, blank
// lines and "#" comment lines allowed (long scenarios exceed the 128 KiB limit of one command-line argument).
#pragma once

#include <cstdint>
#include <string>

namespace kuf::input::script {

// Parses --kuf_input_script. Returns false (with a message) on a syntax error.
bool Start(std::string* error);
void Stop();

// Called on the game thread when game tick `tick` (1-based) starts: applies the input items before it.
void OnGameTick(uint64_t tick);

// Called by the present hook after each frame showing a tick for the first time (tick frame N shows tick N - 1).
void OnFramePresented(uint64_t frame);

// Called by the input hook (game thread, inside a tick) when the game enters a context ("menu", "prompt",
// "gameplay"): schedules the events anchored to it, from the current tick.
void OnContextEntered(const char* context);

// The game tick running now (0 before the first).
uint64_t CurrentTick();

}  // namespace kuf::input::script
