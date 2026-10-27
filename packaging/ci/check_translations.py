#!/usr/bin/env python3
"""Fail when the Ukrainian Qt catalog contains unfinished messages."""
import argparse
import sys
import xml.etree.ElementTree as ET
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--catalog', type=Path, required=True)
    args = parser.parse_args()
    try:
        root = ET.parse(args.catalog).getroot()
    except (OSError, ET.ParseError) as error:
        print(f'translation check: cannot parse {args.catalog}: {error}', file=sys.stderr)
        return 2
    unfinished = []
    for context in root.findall('context'):
        context_name = context.findtext('name', default='<unnamed>')
        for message in context.findall('message'):
            translation = message.find('translation')
            if translation is not None and translation.get('type') == 'unfinished':
                source = message.findtext('source', default='').replace('\n', ' ')
                unfinished.append((context_name, source))
    if unfinished:
        print(f'translation check: {len(unfinished)} unfinished message(s) in {args.catalog}',
              file=sys.stderr)
        for context, source in unfinished:
            print(f'  {context}: {source}', file=sys.stderr)
        return 1
    print(f'translation check: complete ({args.catalog})')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
