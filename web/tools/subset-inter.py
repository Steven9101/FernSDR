#!/usr/bin/env python3
"""Cuts Inter down to what the listener's page prints, for a smaller first visit.

    python3 -m pip install fonttools brotli
    python3 tools/subset-inter.py        # writes src/assets/fonts/inter-latin-wght-400-700.woff2

The page uses weights 400 to 700 and needs kerning and tabular figures; the
fractions, contextual alternates and weights outside that range go. Latin-1
stays whole, since station names and the chat are anyone's language; of the
rest only the marks and arrows the page itself uses are kept (the source has
the up and down arrows but not left and right, which come from the system). Run it again
after @fontsource-variable/inter is updated or the page starts printing a
character this list lacks (it then falls back to the system's sans serif).
"""
import io
import os

from fontTools import subset
from fontTools.ttLib import TTFont
from fontTools.varLib import instancer

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, '..', 'node_modules', '@fontsource-variable', 'inter', 'files', 'inter-latin-wght-normal.woff2')
OUT = os.path.join(HERE, '..', 'src', 'assets', 'fonts', 'inter-latin-wght-400-700.woff2')

UNICODES = list(range(0x20, 0x7F)) + list(range(0xA0, 0x100)) + [
    0x0131, 0x0152, 0x0153, 0x2013, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2026, 0x2032, 0x2033,
    0x20AC, 0x2122, 0x2191, 0x2193, 0x2212,
]

font = TTFont(SRC)
options = subset.Options()
options.layout_features = ['kern', 'tnum', 'ccmp', 'mark', 'mkmk']
options.name_IDs = ['*']
options.notdef_outline = True
options.hinting = False
subsetter = subset.Subsetter(options)
subsetter.populate(unicodes=UNICODES)
subsetter.subset(font)
# The weight axis is limited on a fresh copy: limiting it first leaves the
# subsetter looking for variations of glyphs it has already dropped.
plain = io.BytesIO()
font.flavor = None
font.save(plain)
plain.seek(0)
font = instancer.instantiateVariableFont(TTFont(plain), {'wght': (400, 700)})
font.flavor = 'woff2'
font.save(OUT)
print(f'{OUT}: {os.path.getsize(OUT)} bytes')
