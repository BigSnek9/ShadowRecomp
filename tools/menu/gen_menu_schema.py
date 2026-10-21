"""#132 in-game settings menu: runtime/menu_schema.json (the content, one line per setting) -> runtime/menu_schema.h
(a constexpr table the menu reads), after checking it against what the runtime really has:
  * every ini key the runtime loads (runtime/settings_ui.cpp) is in the menu (or listed in INTERNAL with the reason),
  * every 'ini' item names a real settings::Values field of the right type (runtime/settings_ui.h),
  * every cheat (runtime/cheats.h) and live counter (settings_ui.cpp kLiveWords) is there once,
  * labels / help are filled, options / ranges are sane, 'shown_if' names an item.
    python tools/menu/gen_menu_schema.py           # check + write the header
    python tools/menu/gen_menu_schema.py --check   # check only (exit 1 on a problem)
    python tools/menu/gen_menu_schema.py --reformat   # also rewrite the JSON one line per setting
Adding a setting: one line in the JSON, then run this."""
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
RT = ROOT / 'runtime'
INTERNAL = {'mv_name': "the movement preset's name (movement_presets.ini / F2 Tweak window), not a menu value",
            'hdr_test': 'HDR calibration pattern, session only (debug)',
            'light_debug': 'lighting debug switch, session only (not saved)',
            'light_shadows': '#147: the page Real-time shadows row is light itself; light_shadows stays on (default)',
            'light_replace': 'archived #147 (docs/lighting/ARCHIVED.md): the runtime holds it off / neutral',
            'light_replace_bloom': 'archived #147 (docs/lighting/ARCHIVED.md): the runtime holds it off / neutral',
            'light_baked': 'archived #147 (docs/lighting/ARCHIVED.md): the runtime holds it off / neutral',
            'light_look': 'archived #147 (docs/lighting/ARCHIVED.md): the runtime holds it off / neutral',
            'light_exposure': 'archived #147 (docs/lighting/ARCHIVED.md): the runtime holds it off / neutral',
            'light_shadow_strength': 'archived #147 (docs/lighting/ARCHIVED.md): the runtime holds it off / neutral',
            'light_shadow_tint': 'archived #147 (docs/lighting/ARCHIVED.md): the runtime holds it off / neutral',
            'light_ambient': 'archived #147 (docs/lighting/ARCHIVED.md): the runtime holds it off / neutral',
            'light_saturation': 'archived #147 (docs/lighting/ARCHIVED.md): the runtime holds it off / neutral',
            'light_sun': 'archived #147 (docs/lighting/ARCHIVED.md): the runtime holds it off / neutral',
            'light_sun_custom': 'archived #147 (docs/lighting/ARCHIVED.md): the runtime holds it off / neutral',
            'light_sun_az': 'archived #147 (docs/lighting/ARCHIVED.md): the runtime holds it off / neutral',
            'light_sun_elev': 'archived #147 (docs/lighting/ARCHIVED.md): the runtime holds it off / neutral',
            'light_sun_maxelev': 'archived #147 (docs/lighting/ARCHIVED.md): the runtime holds it off / neutral',
            'light_sky': 'archived #147 (docs/lighting/ARCHIVED.md): the runtime holds it off / neutral',
            'light_profiles': 'archived #147 (docs/lighting/ARCHIVED.md): the runtime holds it off / neutral',
            'light_gi': 'archived #147 (docs/lighting/ARCHIVED.md): the runtime holds it off / neutral',
            'light_gi_bounces': 'archived #147 (docs/lighting/ARCHIVED.md): the runtime holds it off / neutral',
            'light_emissive': 'archived #147 (docs/lighting/ARCHIVED.md): the runtime holds it off / neutral',
            'light_emissive_str': 'archived #147 (docs/lighting/ARCHIVED.md): the runtime holds it off / neutral',
            'light_emissive_range': 'archived #147 (docs/lighting/ARCHIVED.md): the runtime holds it off / neutral',
            'light_glow_flicker': 'archived #147 (docs/lighting/ARCHIVED.md): the runtime holds it off / neutral',
            'light_ao_mode': 'archived #147 (docs/lighting/ARCHIVED.md): the runtime holds it off / neutral',
            'light_ao_quality': 'archived #147 (docs/lighting/ARCHIVED.md): the runtime holds it off / neutral',
            'light_ao_radius': 'archived #147 (docs/lighting/ARCHIVED.md): the runtime holds it off / neutral',
            'light_view': 'archived #147 (docs/lighting/ARCHIVED.md): the runtime holds it off / neutral',
            'light_objects': 'archived #147 (docs/lighting/ARCHIVED.md): the runtime holds it off / neutral',
            'light_object_shadows': 'archived #147 (docs/lighting/ARCHIVED.md): the runtime holds it off / neutral',
            'light_world_only': 'archived #147 (docs/lighting/ARCHIVED.md): the runtime holds it off / neutral',
            'light_blob': 'archived #147 (docs/lighting/ARCHIVED.md): the runtime holds it off / neutral',
            'light_caster_margin': 'archived #147 (docs/lighting/ARCHIVED.md): the runtime holds it off / neutral',
            'hud_169': 'obsolete (#143): always off, the runtime no longer reads it (field to be removed)',
            'gauges_new': 'obsolete (#143): the PC gauges are gone (field to be removed)',
            'parallax': 'saved but not active yet (needs height maps): no dead switch in the menu',
            'parallax_depth': 'goes with parallax (not active yet)',
            'ao': '#148 contact AO, left out by the user for now (the menu shows the greyed light_ao WIP row)',
            'ao_strength': 'goes with ao (#148, not in the menu yet)',
            'clip_ui_43': '#149: always on (hides HUD pieces the game parks just outside 4:3); the one bars toggle is cutscene_bars',
            'pause_new': "retired #149 (user: remove the new pause menu): the game's own pause is always used; old ini lines are ignored",
            'boss_camera_lock': 'custom stages only (user 2026-10-21: no custom stages / editor yet): hidden, default 1 = original',
            'boss_intro_camera': 'custom stages only (user 2026-10-21: no custom stages / editor yet): hidden, default 0',
            'scale': 'older ini spelling of the resolution (height = 480 x scale); the menu writes width / height'}
RESTART = {'none': 0, 'stage': 1, 'game': 2}   # when a change takes effect: live / the next stage / after a game restart
HELP_MAX = 110   # Shadow's look (IN-GAME-MENU-STYLE.md): the help strip wraps to 2 lines at 21 u
KINDS = {'toggle', 'slider', 'choice', 'fps', 'resolution', 'path', 'session', 'live'}


def values_fields():
    """settings::Values members -> 'int' | 'bool' | 'string'"""
    h = (RT / 'settings_ui.h').read_text(encoding='utf-8')
    body = h[h.index('struct Values {') + len('struct Values {'):h.index('};', h.index('struct Values {'))]
    out = {}
    body = ' '.join(line.split('//')[0] for line in body.splitlines())   # declarations may span lines
    for stmt in body.split(';'):
        m = re.match(r'\s*(int|bool|std::string)\s+(.+)', stmt)
        if not m:
            continue
        for part in m.group(2).split(','):
            name = part.split('=')[0].strip()
            if name:
                out[name] = {'int': 'int', 'bool': 'bool', 'std::string': 'string'}[m.group(1)]
    return out


def values_defaults():
    """settings::Values member -> its default from the initialiser in settings_ui.h (bool -> 0 / 1; strings -> '')"""
    h = (RT / 'settings_ui.h').read_text(encoding='utf-8')
    body = h[h.index('struct Values {') + len('struct Values {'):h.index('};', h.index('struct Values {'))]
    body = ' '.join(line.split('//')[0] for line in body.splitlines()); out = {}
    for stmt in body.split(';'):
        m = re.match(r'\s*(int|bool|std::string)\s+(.+)', stmt)
        if not m:
            continue
        for part in m.group(2).split(','):
            name, _, v = (x.strip() for x in part.partition('='))
            if name:
                out[name] = '' if m.group(1) == 'std::string' else (1 if v == 'true' else 0 if v in ('false', '') else int(v))
    return out


def fill_defaults(schema):
    """#141: an ini item's default IS the runtime's (settings_ui.h) - the menu's Reset and the game can never disagree"""
    d = values_defaults(); changed = []
    for p in schema['pages']:
        for sc in p['sections']:
            for i in sc['items']:
                if i.get('store') != 'ini':
                    continue
                ks = i.get('keys') or [i['key']]
                v = [d.get(k) for k in ks] if len(ks) > 1 else d.get(ks[0])
                if v is not None and i.get('default') != v:
                    changed.append(f"{i['key']} {i.get('default')} -> {v}"); i['default'] = v
    return changed


def runtime_keys():
    """every key settings_ui.cpp loads from shadow_settings.ini"""
    c = (RT / 'settings_ui.cpp').read_text(encoding='utf-8')
    keys = set(re.findall(r'\{"(\w+)", &Values::\w+\}', c))
    keys |= set(re.findall(r'!strcmp\(key, "(\w+)"\)', c))
    keys |= set(re.findall(r'strncmp\(line, "(\w+)=", \d+\)', c))
    live = dict(re.findall(r'\{"(\w+)", (0x8057[0-9A-Fa-f]+)\}', c))   # key -> RAM word
    return keys - set(live) - {'unlock_all_stages'}, live


def cheat_ids():
    h = (RT / 'cheats.h').read_text(encoding='utf-8')
    body = h[h.index('enum Id {'):h.index('COUNT')]
    return [m for m in re.findall(r'^\s*([A-Z0-9_]+),', body, re.M)]


def check(schema):
    errs = []
    fields = values_fields()
    rkeys, live = runtime_keys()
    ids = cheat_ids()
    items = [(p, s, i) for p in schema['pages'] for s in p['sections'] for i in s['items']]
    seen = {}
    for p, s, i in items:
        k = i.get('key')
        where = f"{p['id']}/{s['title']}/{k}"
        if not k:
            errs.append(f'{where}: no key'); continue
        if k in seen:
            errs.append(f'{where}: key also in {seen[k]}')
        seen[k] = where
        if i.get('kind') not in KINDS:
            errs.append(f"{where}: kind {i.get('kind')!r} not one of {sorted(KINDS)}")
        if not i.get('label'):
            errs.append(f'{where}: no label')
        if not i.get('help'):
            errs.append(f'{where}: no one-line help')
        elif len(i['help']) > HELP_MAX:
            errs.append(f"{where}: help is {len(i['help'])} characters (the menu's help strip fits {HELP_MAX}: 2 lines at 21 u)")
        if i.get('restart') not in RESTART:
            errs.append(f"{where}: restart must be none / stage (next stage) / game (after a game restart)")
        st = i.get('store')
        if st == 'ini':
            for fk in i.get('keys') or [k]:
                if fk not in fields:
                    errs.append(f'{where}: no settings::Values field {fk}')
            want = {'toggle': ('bool', 'int'), 'path': ('string',)}.get(i['kind'], ('int', 'bool'))
            for fk in i.get('keys') or [k]:
                if fk in fields and fields[fk] not in want:
                    errs.append(f"{where}: field {fk} is {fields[fk]}, kind {i['kind']} wants {want}")
        elif st == 'cheats':
            if k != 'unlock_all_stages' and not (isinstance(i.get('cheat'), int) and 0 <= i['cheat'] < len(ids)):
                errs.append(f'{where}: cheat id missing or out of range (0..{len(ids) - 1})')
        elif st == 'live':
            if k not in live:
                errs.append(f'{where}: not a live counter of settings_ui.cpp ({sorted(live)})')
        else:
            errs.append(f'{where}: store must be ini / cheats / live')
        if i['kind'] in ('choice', 'fps', 'resolution') and not i.get('options'):
            errs.append(f'{where}: no options')
        if i['kind'] in ('slider', 'live') and not (isinstance(i.get('lo'), int) and isinstance(i.get('hi'), int) and i['lo'] < i['hi']):
            errs.append(f'{where}: slider needs lo < hi')
        if i['kind'] == 'choice' and 'default' in i and i['default'] not in [o[1] for o in i['options']]:
            errs.append(f"{where}: default {i['default']} is not one of the options")
    keys = set(seen)
    for p, s, i in items:
        r = i.get('shown_if')
        if r and r.split('=')[0] not in keys:
            errs.append(f"{p['id']}/{s['title']}/{i['key']}: shown_if names an unknown key {r}")
    covered = keys | {fk for _, _, i in items for fk in i.get('keys') or []}
    for k in sorted(rkeys - covered - set(INTERNAL)):
        errs.append(f'runtime key {k} (settings_ui.cpp) is not in the menu - add it or list it in INTERNAL with the reason')
    for k in sorted(set(live) - keys):
        errs.append(f'live counter {k} is not in the menu')
    have = {i.get('cheat') for _, _, i in items if i.get('store') == 'cheats'}
    for n, name in enumerate(ids):
        if n not in have:
            errs.append(f'cheat {n} {name} (cheats.h) is not in the menu')
    if 'unlock_all_stages' not in keys:
        errs.append('unlock_all_stages is not in the menu')
    return errs, len(items), len(rkeys)


def c_str(s):
    return 'nullptr' if s is None else json.dumps(s, ensure_ascii=True)


def header(schema):
    fields = values_fields()
    live = runtime_keys()[1]
    L = ['#pragma once',
         '// GENERATED by tools/menu/gen_menu_schema.py from runtime/menu_schema.json - edit the JSON, not this file.',
         '// #132 in-game settings menu content: pages > sections > items. The menu draws them; an item',
         '// edits settings::g through its field (store Ini, then settings::save()), a cheat through cheats::enabled(cheat)',
         '// (session only), a live counter through its RAM word (settings_ui.cpp kLiveWords).',
         '#include <string>',
         '#include "settings_ui.h"',
         '',
         'namespace menu_schema {',
         'enum class Kind { Toggle, Slider, Choice, Fps, Resolution, Path, Session, Live };',
         'enum class Store { Ini, Cheats, Live };',
         'struct Option { const char* label; int value; int value2; };   // value2: Resolution height',
         'struct Page { const char* id; const char* title; };',
         'struct Section { int page; const char* title; bool advanced; const char* note; };   // note: a banner line under the section title (#145)',
         'struct Item {',
         '  int section; const char* key; Kind kind; Store store; const char* label; const char* help;',
         '  int def, lo, hi; const char* fmt; const Option* options; int n_options;',
         '  int restart;                  // 0 live, 1 from the next stage ("next stage" tag), 2 after a game restart ("after restart")',
         '  const char* shown_if; int shown_if_value;   // shown / enabled only when item `shown_if` has this value (-1 = any non-zero)',
         '  int cheat;                    // Store::Cheats: cheats::Id (-1 = unlock_all_stages)',
         '  int scale;                    // Store::Live: shown as raw * 100 / scale % (scale = the raw value of 100%, gauges 30000); 0 = raw',
         '  unsigned addr;                // Store::Live: the s32 RAM word (settings_ui.cpp kLiveWords)',
         '  int settings::Values::* ifield; int settings::Values::* ifield2; bool settings::Values::* bfield; std::string settings::Values::* sfield;',
         '  bool experimental;            // #145: draw an "Experimental" warning tag next to it (unfinished / untested)',
         '  bool wip;                     // #147: draw it greyed out with a "Work in progress" tag; it cannot be changed',
         '  int step;                     // #150: raw units per left / right press (0 = the menu picks); live counters: exact steps',
         '};', '']
    pages, sections, items = [], [], []
    opt_n = 0
    for p in schema['pages']:
        pages.append(f"    {{{c_str(p['id'])}, {c_str(p['title'])}}},")
        for s in p['sections']:
            sections.append(f"    {{{len(pages) - 1}, {c_str(s['title'])}, {'true' if s.get('advanced') else 'false'}, {c_str(s.get('note'))}}},")
            for i in s['items']:
                opts = 'nullptr'
                n = len(i.get('options') or [])
                if n:
                    name = f'kOpt{opt_n}'; opt_n += 1; opts = name
                    rows = ', '.join(f"{{{c_str(o[0])}, {int(o[1])}, {int(o[2]) if len(o) > 2 else 0}}}" for o in i['options'])
                    L.append(f'inline constexpr Option {name}[] = {{{rows}}};   // {i["key"]}')
                kind = {'toggle': 'Toggle', 'slider': 'Slider', 'choice': 'Choice', 'fps': 'Fps', 'resolution': 'Resolution', 'path': 'Path',
                        'session': 'Session', 'live': 'Live'}[i['kind']]
                store = {'ini': 'Ini', 'cheats': 'Cheats', 'live': 'Live'}[i['store']]
                d = i.get('default', 0)
                d = d[0] if isinstance(d, list) else (int(d) if isinstance(d, (int, bool)) else 0)
                req, rv = (i.get('shown_if') or '').partition('=')[::2] if i.get('shown_if') else ('', '')
                fi = fi2 = fb = fs = 'nullptr'
                if i['store'] == 'ini':
                    ks = i.get('keys') or [i['key']]
                    t = fields[ks[0]]
                    if t == 'int':
                        fi = f'&settings::Values::{ks[0]}'
                        if len(ks) > 1:
                            fi2 = f'&settings::Values::{ks[1]}'
                    elif t == 'bool':
                        fb = f'&settings::Values::{ks[0]}'
                    else:
                        fs = f'&settings::Values::{ks[0]}'
                cheat = i.get('cheat', -1) if i['store'] == 'cheats' else -1
                items.append(f"    {{{len(sections) - 1}, {c_str(i['key'])}, Kind::{kind}, Store::{store}, {c_str(i['label'])}, {c_str(i['help'])}, "
                             f"{d}, {int(i.get('lo', 0))}, {int(i.get('hi', 0))}, {c_str(i.get('fmt'))}, {opts}, {n}, "
                             f"{RESTART[i.get('restart', 'none')]}, {c_str(req or None)}, {int(rv) if rv else -1}, {cheat}, "
                             f"{int(i.get('scale', 0))}, {live.get(i['key'], '0') if i['store'] == 'live' else '0'}u, {fi}, {fi2}, {fb}, {fs}, {'true' if i.get('experimental') else 'false'}, {'true' if i.get('wip') else 'false'}, {int(i.get('step', 0))}}},")
    L += ['', 'inline constexpr Page kPages[] = {'] + pages + ['};', 'inline constexpr Section kSections[] = {'] + sections + ['};',
          'inline constexpr Item kItems[] = {'] + items + ['};',
          'inline constexpr int kNumPages = int(sizeof kPages / sizeof kPages[0]);',
          'inline constexpr int kNumSections = int(sizeof kSections / sizeof kSections[0]);',
          'inline constexpr int kNumItems = int(sizeof kItems / sizeof kItems[0]);',
          '}  // namespace menu_schema', '']
    return '\n'.join(L)


# the menu file (#132): runtime/menu/settings_menu.txt, one setting per line
#   page|key|label|type|restart|help      restart: 0 live, 1 next stage, 2 after a game restart; types: bool | int:min:max:step | choice:v=Label,... | action:<id>
# plus three types the content needs (agreed in AGENTS-LIVE): path (a folder, string key), cheat (session cheat:
# key cheatN or unlock_all_stages -> cheats::enabled / settings::g.unlock_all_stages) and live:min:max:step (a RAM
# counter: rings, lives, scores, gauges). '## Title' = a subheading; '#' = comment.
SYSTEM = [('restart', 'Restart the game', 'Settings tagged "after restart" take effect after it.'),
          ('reset_page', 'Reset this page', 'Puts every setting on the current page back to its default.')]


def txt_clean(t):   # a choice option label: no field / list separators in it
    return str(t).replace('|', '/').replace(', ', ' / ').replace(',', ' ').replace('=', '-').strip()


def step_for(lo, hi):
    span = hi - lo
    return 1 if span <= 20 else 5 if span <= 400 else 10 if span <= 1000 else 50 if span <= 2000 else 100


def menu_txt(schema):
    order = [p['title'] for p in schema['pages']]   # #140: the schema's pages, in its order
    if 'System' not in order: order.append('System')   # #170: the schema may hold the System page's own settings
    rows = {p: [] for p in order}
    for p in schema['pages']:
        page = p['title']
        for sc in p['sections']:
            rows[page].append(f"## {sc['title']}")
            if sc.get('note'):
                rows[page].append(f"# {sc['note']}")
            for i in sc['items']:
                k, kind = i['key'], i['kind']
                if kind == 'toggle':
                    t = 'bool'
                elif kind == 'slider':
                    t = f"int:{i['lo']}:{i['hi']}:{step_for(i['lo'], i['hi'])}"
                elif kind in ('choice', 'fps'):
                    t = 'choice:' + ','.join(f'{o[1]}={txt_clean(o[0])}' for o in i['options'])
                elif kind == 'resolution':   # the render height decides it (the width follows the aspect)
                    k = 'height'
                    t = 'choice:' + ','.join(f'{o[2]}={txt_clean(o[0])}' for o in i['options'])
                elif kind == 'path':
                    t = 'path'
                elif kind == 'session':
                    k = 'unlock_all_stages' if i.get('cheat') is None else f"cheat{i['cheat']}"
                    t = 'cheat'
                elif kind == 'live':
                    t = f"live:{i['lo']}:{i['hi']}:{i.get('scale') or 1}"
                else:
                    raise ValueError(kind)
                rows[page].append('|'.join([page, k, i['label'].replace('|', '/'), t, str(RESTART[i.get('restart', 'none')]), (('[Work in progress] ' if i.get('wip') else '[Experimental] ' if i.get('experimental') else '') + i['help']).replace('|', '/')]))
    rows['System'] += ['## Game'] + [f'System||{lab}|action:{a}|0|{h}' for a, lab, h in SYSTEM]
    out = ['# GENERATED by tools/menu/gen_menu_schema.py from runtime/menu_schema.json - edit the JSON, not this file.',
           '# #132 in-game settings menu (the game reads it at boot): page|key|label|type|restart|help']
    for p in order:
        out += rows[p]
    return chr(10).join(out) + chr(10)


def dump_json(schema):
    """the schema file layout: pages and sections on their own lines, one line per setting"""
    out = ['{', f'  "version": {schema["version"]},', f'  "about": {json.dumps(schema["about"])},', '  "pages": [']
    for pi, p in enumerate(schema['pages']):
        out.append(f'    {{"id": {json.dumps(p["id"])}, "title": {json.dumps(p["title"])}, "sections": [')
        for si, sc in enumerate(p['sections']):
            head = {k: v for k, v in sc.items() if k != 'items'}
            out.append('      {' + ', '.join(f'{json.dumps(k)}: {json.dumps(v)}' for k, v in head.items()) + ', "items": [')
            for ii, i in enumerate(sc['items']):
                out.append('        ' + json.dumps(i, ensure_ascii=False) + (',' if ii < len(sc['items']) - 1 else ''))
            out.append('      ]}' + (',' if si < len(p['sections']) - 1 else ''))
        out.append('    ]}' + (',' if pi < len(schema['pages']) - 1 else ''))
    return chr(10).join(out + ['  ]', '}']) + chr(10)


def main():
    schema = json.loads((RT / 'menu_schema.json').read_text(encoding='utf-8'))
    for c in fill_defaults(schema):
        print('default from settings_ui.h:', c)
    if '--reformat' in sys.argv:   # back to one line per setting (after a script edited the JSON)
        (RT / 'menu_schema.json').write_text(dump_json(schema), encoding='utf-8', newline=chr(10))
    errs, n, nr = check(schema)
    for e in errs:
        print('PROBLEM', e)
    print(f"{n} items on {len(schema['pages'])} pages; {nr} runtime ini keys checked; {len(errs)} problem(s)")
    if errs:
        sys.exit(1)
    if '--check' not in sys.argv:
        (RT / 'menu_schema.h').write_text(header(schema), encoding='utf-8', newline='\n')
        (RT / 'menu').mkdir(exist_ok=True)
        (RT / 'menu' / 'settings_menu.txt').write_text(menu_txt(schema), encoding='utf-8', newline='\n')
        print('wrote runtime/menu_schema.h and runtime/menu/settings_menu.txt')


if __name__ == '__main__':
    main()
