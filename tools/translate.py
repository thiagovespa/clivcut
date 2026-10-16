#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Fill the unfinished entries of the translation files.

Runs `update_translations` (lupdate), then fills every unfinished message of
i18n/clivcut_pt_BR.ts and i18n/clivcut_es.ts:

* from the table given on stdin, one entry per line:
      English|Português|Español
  plural entries (sources containing %n) give the singular and plural forms:
      %n clip(s)|%n clipe|%n clipes|%n clip|%n clips
  and also fill i18n/clivcut_en.ts;
* otherwise, from an existing translation of the same source in another context.

Untranslated entries are listed at the end. Usage:

    python3 tools/translate.py [build-dir] < new-strings.txt
"""
import subprocess
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUILD = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / 'build'
LANGS = {'pt_BR': 0, 'es': 1}


def update():
    subprocess.run(['cmake', '--build', str(BUILD), '--target', 'update_translations'], check=True,
                   capture_output=True)


def read_table():
    simple, plural = {}, {}
    for line in sys.stdin:
        line = line.rstrip('\n')
        if not line.strip():
            continue
        parts = line.split('|')
        if '%n' in parts[0] and len(parts) == 5:
            src = parts[0]
            # Forma em inglês: "(s)" some no singular e vira "s" no plural.
            en = [src.replace('(s)', ''), src.replace('(s)', 's')]
            plural[src] = {'en': en, 'pt_BR': parts[1:3], 'es': parts[3:5]}
        elif len(parts) == 3:
            simple[parts[0]] = (parts[1], parts[2])
        else:
            sys.exit(f'bad line (expected 3 or 5 fields): {line}')
    return simple, plural


def write(tree, path):
    ET.indent(tree, '    ')
    with open(path, 'wb') as f:
        f.write(b'<?xml version="1.0" encoding="utf-8"?>\n<!DOCTYPE TS>\n')
        tree.write(f, encoding='utf-8')
        f.write(b'\n')


def fill(lang, simple, plural):
    path = ROOT / 'i18n' / f'clivcut_{lang}.ts'
    tree = ET.parse(path)
    known = {}
    for msg in tree.iter('message'):
        tr = msg.find('translation')
        if tr.get('type') is None and msg.get('numerus') != 'yes' and tr.text and msg.find('comment') is None:
            known.setdefault(msg.find('source').text or '', tr.text)
    missing = []
    for msg in tree.iter('message'):
        tr = msg.find('translation')
        if tr.get('type') != 'unfinished':
            continue
        src = msg.find('source').text or ''
        if msg.get('numerus') == 'yes':
            if src not in plural:
                missing.append(src)
                continue
            for f in list(tr):
                tr.remove(f)
            for form in plural[src][lang]:
                ET.SubElement(tr, 'numerusform').text = form
        elif lang != 'en' and (src in simple or src in known):
            tr.text = simple[src][LANGS[lang]] if src in simple else known[src]
        else:
            if lang != 'en':
                missing.append(src)
            continue
        del tr.attrib['type']
    write(tree, path)
    return missing


def main():
    simple, plural = read_table()
    update()
    report = {lang: fill(lang, simple, plural) for lang in ('en', 'pt_BR', 'es')}
    update()  # normaliza o formato dos arquivos
    for lang, missing in report.items():
        if missing:
            print(f'{lang}: {len(missing)} untranslated')
            for m in missing:
                print(f'  {m}')
    if not any(report.values()):
        print('all translations complete')


if __name__ == '__main__':
    main()
