#pragma once
// #135 bug capture: F12 or pad Back + Start -> <game>\bugs\<date>_<time>\ (screen.png, state.json, settings.ini,
// log.txt) and a toast. tools/bug_replay.py turns a capture into a test scenario.
namespace bug_capture {
void trigger();   // input.cpp: the key / button pair
void tick();      // every frame (settings::draw): the screenshot is converted once Aurora wrote it
}
