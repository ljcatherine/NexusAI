#!/usr/bin/env python3
"""Change ONLY an isolated ignored NexusAI build tree's title identity for a safe PS5 test.

Runs before the title build. Requires an explicit --test-title-id, refuses the
upstream title identity and refuses tracked or non-forked source repositories.
Names the title "NexusAI" and gives it the Orbit N icon (assets/nexus-icon0.png,
512x512, rendered from assets/nexus-icon.svg). ShadowMountPlus copies the icon
and name only when it registers a title, so an installed title needs a
remove-and-reinstall to show a change.
The PS5's `/data/PS5LM` state is still shared; back it up separately.
"""
import argparse
import json
import pathlib
import re
import shutil
import sys

ORIGINAL = 'PPSA99581'
TITLE_NAME = 'NexusAI'
# The content ID label the installed PPSA99743 test title was registered with.
CONTENT_LABEL = 'NEXUSPS5LM000001'
ICON = pathlib.Path(__file__).resolve().parent / 'assets/nexus-icon0.png'
PNG_MAGIC = b'\x89PNG\r\n\x1a\n'

def prepare(repo: pathlib.Path, test_id: str, icon: pathlib.Path = ICON) -> dict:
    if not re.fullmatch(r'PPSA[0-9]{5}', test_id) or test_id == ORIGINAL:
        raise ValueError('provide a distinct, well-formed test title ID (not the original)')
    repo = repo.resolve(strict=True)
    if repo.name.startswith('.') or repo == pathlib.Path.cwd().resolve():
        raise ValueError('refusing current/original repository')
    # Restrict to generated clones: prepare-build.sh's build/nexusai, the Docker
    # build's /work/nexusai, or an isolated build/ps5lm-nexus-* test clone.
    generated = (repo.parent.name in ('build', 'work') and repo.name == 'nexusai') or \
        (repo.parent.name == 'build' and repo.name.startswith('ps5lm-nexus-'))
    if not generated:
        raise ValueError('target must be a generated build/nexusai or build/ps5lm-nexus-* clone')
    script = repo / 'scripts/build-app.sh'
    target = repo / 'ps5/app/sce_sys/param.json'
    icon_target = repo / 'ps5/app/sce_sys/icon0.png'
    if not script.is_file() or not target.is_file() or target.is_symlink() or icon_target.is_symlink():
        raise ValueError('not a NexusAI native title build tree')
    data = icon.read_bytes()
    # A 512x512 PNG: the size of the upstream icon0.png and what the launcher expects.
    if data[:8] != PNG_MAGIC or data[12:16] != b'IHDR' or data[16:24] != (512).to_bytes(4, 'big') * 2:
        raise ValueError('icon must be a 512x512 PNG')
    meta = json.loads(target.read_text())
    if meta.get('titleId') not in (ORIGINAL, test_id):
        raise ValueError('unexpected input title ID; refuse to rewrite')
    meta['titleId'] = test_id
    meta['conceptId'] = test_id[4:]
    meta['contentId'] = f'UP9000-{test_id}_00-{CONTENT_LABEL}'
    meta.setdefault('localizedParameters', {}).setdefault('en-US', {})['titleName'] = TITLE_NAME
    if len(meta['contentId']) != 36:
        raise ValueError('incorrect contentId length')
    target.write_text(json.dumps(meta, indent=4) + '\n')
    shutil.copyfile(icon, icon_target)
    result = {k: meta[k] for k in ('titleId', 'contentId', 'conceptId')}
    result['titleName'] = TITLE_NAME
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('clone', type=pathlib.Path)
    parser.add_argument('--test-title-id', required=True, help='distinct ID confirmed unused on your PS5')
    args = parser.parse_args()
    print(json.dumps(prepare(args.clone, args.test_title_id), indent=2))

if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError, json.JSONDecodeError) as error:
        print('Refusing test title identity change:', error, file=sys.stderr)
        sys.exit(1)
