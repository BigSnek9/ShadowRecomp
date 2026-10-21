"""Enriches catalog.json from ShadowSET (research snapshot, see README attribution): enum choices for fields (every
ShadowSET enum is `: int`, one 4-byte parameter word), fields of hand-written readers, and from ShadowObjectList.ini
the class names + the DFF models each class draws (`models`: alternative sets; `modelSetting`: the parameter field that
picks the set, e.g. BoxType). Classes missing from the catalog are added with their name.
Run: python tools/stage_editor/enrich_catalog.py   (rewrites catalog.json in place; idempotent)."""
import json, re
from pathlib import Path

APP = Path(__file__).resolve().parent
SRC = APP.parents[1] / 'work/stage-editor-research/ShadowSET/ShadowSET/SetObjects/ObjectClassesShadow'
ENUM = re.compile(r'enum\s+(\w+)\s*(?::\s*\w+)?\s*\{([^}]*)\}', re.S)
PROP = re.compile(r'\[MiscSetting[^\]]*\]\s*public\s+(\w+)\s+(\w+)\s*\{', re.S)


OVERRIDES = {
    (0x00, 0x3A, 'Weapon'): dict(label='Fallback weapon', help='Special Weapon Box: gives the special weapon your save has unlocked; this weapon is only used if special weapons are not unlocked yet (ShadowSET).'),
}
# classes the snapshot has no reader for; read order from the game (#124, docs/stage-workshop/research/spring-arcs-124.md)
EXTRA_FIELDS = {
    (0x00, 0x06): [dict(name='Strength', type='float', label='Power', help='Launch strength.', group='Basic'),
                   dict(name='NoControlTime', type='float', label='No control', help='Time the stick does nothing after the launch.', unit='seconds', group='Basic')],
    (0x07, 0xDE): [dict(name='Strength', type='float', label='Power', help='Launch strength.', group='Basic'),
                   dict(name='NoControlTime', type='float', label='No control', help='Time the stick does nothing after the launch.', unit='seconds', group='Basic')],
}


def parse_enums(text):
    out = {}
    for name, body in ENUM.findall(text):
        value, opts = -1, []
        for item in body.split(','):
            item = re.sub(r'//.*', '', item).strip()
            if not item:
                continue
            if '=' in item:
                key, v = [x.strip() for x in item.split('=', 1)]
                try:
                    value = int(v, 0)
                except ValueError:
                    continue
            else:
                key, value = item, value + 1
            opts.append([value, re.sub(r'(?<=[a-z])(?=[A-Z])', ' ', key)])
        out[name] = opts
    return out


def main():
    shared = parse_enums((SRC / 'Enums.cs').read_text('utf-8-sig'))
    # missing from the snapshot; names from ShadowSET's comments, order as in Heroes' ring types
    shared.setdefault('ERingType', [[0, 'Normal / single'], [1, 'Line'], [2, 'Circle'], [3, 'Arch']])
    files = {p.name: p for p in SRC.rglob('*.cs')}
    catalog = json.loads((APP / 'catalog.json').read_text('utf-8'))
    added = 0
    for entry in catalog:
        p = files.get(entry.get('source', ''))
        if not p:
            continue
        text = p.read_text('utf-8-sig')
        enums = {**shared, **parse_enums(text)}
        types = dict((name, typ) for typ, name in PROP.findall(text))
        if not entry.get('fields'):
            # classes with a hand-written ReadMiscSettings: fields in read order (int / float / bool / enum casts)
            body = re.search(r'ReadMiscSettings\(BinaryReader reader, int count\)\s*\{(.*?)\n        \}', text, re.S)
            fields = []
            for m in re.finditer(r'(\w+)\s*=\s*(?:\((\w+)\))?\s*reader\.Read(Int32|Single|UInt32)\(\)\s*(!=\s*0)?', body.group(1) if body else ''):
                name, cast, kind, boolean = m.groups()
                typ = 'float' if kind == 'Single' else 'bool' if boolean else 'int'
                f = dict(name=name, type=typ, order=len(fields) + 1)
                if cast and cast in enums and enums[cast]:
                    f['type'] = 'enum'; f['options'] = enums[cast]
                if typ == 'bool':
                    f['type'] = 'enum'; f['options'] = [[0, 'No'], [1, 'Yes']]
                fields.append(f)
            if fields:
                entry['fields'] = fields
                added += len(fields)
        # [Description("..")] alone or combined, e.g. [MiscSetting, Description("..")] (113 fields use the combined form)
        helps = dict((name, text) for text, name in re.findall(r'\[[^\]]*?Description\("([^"]*)"\)[^\]]*\]\s*(?:\[[^\]]*\]\s*)*public\s+\w+\s+(\w+)', text))
        for f in entry.get('fields', []):
            if f['name'] in helps: f['help'] = helps[f['name']]
        for f in entry.get('fields', []):
            typ = types.get(f['name'])
            if typ in enums and enums[typ]:
                f['type'] = 'enum'
                f['options'] = enums[typ]
                added += 1
    ini = APP.parents[1] / 'work/stage-editor-research/ShadowSET-lists/ShadowObjectList.ini'
    if ini.exists():
        entries, cur = {}, None
        for line in ini.read_text('utf-8-sig').splitlines():
            m = re.match(r'\[([0-9A-Fa-f]{2})\]\[([0-9A-Fa-f]{2})\]', line)
            if m:
                cur = entries.setdefault((int(m.group(1), 16), int(m.group(2), 16)), {'models': []})
                continue
            if cur is None or '=' not in line:
                continue
            k, v = line.split('=', 1)
            if k == 'Object': cur['name'] = v.strip()
            elif k == 'Model': cur['models'].append([x.strip() for x in v.split(',') if x.strip()])
            elif k == 'ModelMiscSetting': cur['modelSetting'] = v.strip()
        by_key = {(e['list'], e['type']): e for e in catalog}
        for key, e in entries.items():
            if key == (0, 0):
                continue
            entry = by_key.get(key)
            if entry is None:
                entry = dict(list=key[0], type=key[1], name=e.get('name', f'Object {key[0]:02X}{key[1]:02X}'), fields=[], source='ShadowObjectList.ini')
                catalog.append(entry); by_key[key] = entry
            if e['models']: entry['models'] = e['models']
            if e.get('modelSetting'): entry['modelSetting'] = e['modelSetting']
            entry.setdefault('listName', e.get('name'))
        catalog.sort(key=lambda e: (e['list'], e['type']))
    for e in catalog:
        if not e.get('fields') and (e['list'], e['type']) in EXTRA_FIELDS:
            e['fields'] = [dict(f, order=i + 1, src='data') for i, f in enumerate(EXTRA_FIELDS[(e['list'], e['type'])])]
    # labels where the plain field name misleads (checked against ShadowSET's descriptions)
    for e in catalog:
        for f in e.get('fields', []):
            o = OVERRIDES.get((e['list'], e['type'], f['name']))
            if o: f.update(o)
    (APP / 'catalog.json').write_text(json.dumps(catalog, ensure_ascii=False, indent=1), 'utf-8')
    print(f'{added} enum fields')


if __name__ == '__main__':
    main()
