"""Author Filo's three original 24-unit vector plugins. No third-party artwork.
Run from the repository root. Manifests are the runtime assets; this is authoring only.
"""
from pathlib import Path
import json, re

ROOT=Path(__file__).resolve().parents[1]
classic={n:(p,f=='true') for n,p,f in re.findall(r'\{"([a-z]+)",\s*"([^"]+)", (true|false)\}',(ROOT/'src/gfx/icons.cpp').read_text(encoding='utf-8'))}
def line(p, width=1.7, opacity=1):return dict(path=p,fill=False,stroke=width,opacity=opacity)
def fill(p, opacity=1):return dict(path=p,fill=True,opacity=opacity)
def table(s):return dict(row.split('|',1) for row in s.strip().splitlines())
# Circuit: square terminals, right-angle routes, explicit input/output and signal flow.
circuit=table('''compress|M3 3h6v2H5v4H3zM15 19h4v-4h2v6h-6zM6 6l4 4M6 10h4V6M18 18l-4-4M14 18v-4h4
metadata|M4 3h12l4 4v14H4zM16 3v4h4M7 10h2M11 10h6M7 14h2M11 14h6M7 18h2M11 18h4
edit|M3 6h8M15 6h6M3 12h3M10 12h11M3 18h11M18 18h3M11 4h4v4h-4zM6 10h4v4H6zM14 16h4v4h-4z
annotate|M3 4h15v5M3 4v16h16v-8M8 16v-4l9-9 4 4-9 9zM15 5l4 4
addbg|M3 5h18v14H3zM3 15l5-5 5 5 3-3 5 5M7 8h2M16 3v5M13.5 5.5h5
crop|M7 2v15h15M2 7h15v15M10 10h4v4
redact|M3 4h18v16H3zM6 8h12M6 12h12M6 16h7M2 2l20 20
trim|M3 4h4v4H3zM3 16h4v4H3zM7 6h3l8 12h3M7 18h3l8-12h3M12 3v3M12 18v3
speed|M3 19V9l6-6h6l6 6v10M6 19h12M12 15l5-6M10 15h4M6 10l2 2M16 5l-1 3
join|M2 6h7v12H2zM15 6h7v12h-7zM9 12h6M11 9l3 3-3 3
frames|M3 8h13v13H3zM8 3h13v13M6 12h7M6 16h7M11 6h7
split|M12 2v20M9 12H3M6 9l-3 3 3 3M15 12h6M18 9l3 3-3 3M10 5h4M10 19h4
normalize|M3 8v8M7 5v14M11 10v4M15 3v18M19 9v6M2 12h20
visualizer|M2 12h4V5h4v14h4V8h4v4h4
channels|M3 3h7v18H3zM14 3h7v18h-7zM5 9h3M5 12h3M5 15h3M16 9h3M16 12h3M16 15h3
bleep|M3 8h4l5-4v16l-5-4H3zM16 6h5v12h-5zM16 10h5M16 14h5
merge|M2 4h6v5H2zM2 15h6v5H2zM8 6.5h4V17.5H8M12 12h4M16 9h6v6h-6z
organize|M3 3h7v7H3zM14 3h7v7h-7zM3 14h7v7H3zM14 14h7v7h-7zM10 6h4M6 10v4M18 10v4M10 18h4
share|M12 15V3M8 7l4-4 4 4M4 11v10h16V11M7 17h10
paperclip|M7 17V6l3-3h8l3 3v12l-3 3H6l-3-3V8M11 16V7h5v9h-5
float|M3 4h13v10H3zM8 9h13v11H8zM13 14h4M15 12v4
copy|M3 3h12v4M3 3v12h4M8 8h13v13H8zM11 12h7M11 16h7
move|M3 7V3h4M17 3h4v4M3 17v4h4M21 17v4h-4M5 12h14M8 9l-3 3 3 3M16 9l3 3-3 3
copypath|M3 3h6v6H3zM15 15h6v6h-6zM9 6h9v9M6 9v9h9
terminal|M2 4h20v16H2zM2 8h20M5 6h1M8 6h1M5 11l3 3-3 3M11 17h6
convert|M2 8h18M16 4l4 4-4 4M22 16H4M8 12l-4 4 4 4
extract|M3 3h18v5H3zM5 8v13h14V8M12 11v7M9 15l3 3 3-3
archive|M3 3h18v5H3zM5 8v13h14V8M10 11h4v3h-4z
qr|M2 2h8v8H2zM14 2h8v8h-8zM2 14h8v8H2zM5 5h2v2H5zM17 5h2v2h-2zM5 17h2v2H5zM14 14h3v3h5v5h-4M14 19v3M21 14v2
removeaudio|M3 8h4l5-4v16l-5-4H3zM16 8l6 8M22 8l-6 8
rotate|M4 17V7l4-4h9l4 4v10l-4 4h-5M3 11h5M3 11V6M9 7v10h7V7z
trash|M4 6h16M8 6V3h8v3M6 6v15h12V6M10 10v7M14 10v7
play|M6 3l15 9-15 9z
pause|M5 3h5v18H5zM14 3h5v18h-5z
stop|M4 4h16v16H4z
close|M4 4l16 16M20 4L4 20
reset|M4 10V6l3-3h10l4 4v10l-4 4H8l-4-4M4 4v6h6
zoom|M3 3h12v12H3zM15 15l6 6
chevronup|M4 16V12l8-8 8 8v4l-8-8z
chevrondown|M4 8v4l8 8 8-8V8l-8 8z
chevronleft|M16 4h-4l-8 8 8 8h4l-8-8z
chevronright|M8 4h4l8 8-8 8H8l8-8z
folder|M2 5h7l3 3h10v13H2zM2 11h20
image|M2 4h20v16H2zM2 16l6-7 6 6 3-3 5 5M16 7h3v3h-3z
video|M2 4h20v16H2zM6 4v16M18 4v16M2 8h4M2 16h4M18 8h4M18 16h4M10 9l5 3-5 3z
audio|M4 5h16v14H4zM7 9v6M10 7v10M13 10v4M16 8v8
document|M4 2h11l5 5v15H4zM15 2v5h5M7 11h10M7 15h10M7 19h6
check|M3 12l6 6L21 6M3 17v4h4M17 3h4v4
warning|M9 3h6l7 14v4H2v-4zM12 8v6M11 17h2
more|M3 10h4v4H3zM10 10h4v4h-4zM17 10h4v4h-4z
add|M10 3h4v7h7v4h-7v7h-4v-7H3v-4h7z
remove|M3 10h18v4H3z
grip|M6 4h4v4H6zM14 4h4v4h-4zM6 10h4v4H6zM14 10h4v4h-4zM6 16h4v4H6zM14 16h4v4h-4z
mono|M4 4h16v16H4zM12 4v16M12 8h4M12 12h4M12 16h4
pin|M7 3h10v3h-2v5l3 3v2H6v-2l3-3V6H7zM12 16v6
settings|M2 4h20v16H2zM6 8h12M6 12h12M6 16h12M9 6v4M15 10v4M10 14v4
star|M12 2l3 7h7l-6 5 2 8-6-5-6 5 2-8-6-5h7z
swap|M6 3L2 7l4 4M2 7h17v4M18 13l4 4-4 4M22 17H5v-4
layers|M3 3h13v13H3zM8 8h13v13H8zM8 12h8M12 8v8
text|M3 7V3h18v4M12 3v18M8 21h8
arrow|M3 21L21 3M8 3h13v13
line|M3 21L21 3M2 18v4h4M18 2h4v4
rect|M3 5h18v14H3zM3 3v4M21 3v4M3 17v4M21 17v4
circle|M8 3h8l5 5v8l-5 5H8l-5-5V8z
pen|M3 21v-6L16 2l6 6L9 21zM13 5l6 6M3 15l6 6
highlighter|M7 16l9-13 6 5-10 13H7zM4 21h13M12 9l6 5
numbered|M3 3h18v18H3zM9 9l3-2v10M9 17h6
magnifier|M3 3h12v12H3zM15 15l6 6M6 9h6M9 6v6''')
# Pebble: broad rounded silhouettes with a solid semantic foreground and soft internal mass.
body=table('''compress|M3 7Q3 3 7 3h10q4 0 4 4v10q0 4-4 4H7q-4 0-4-4z
metadata|M3 6q0-3 3-3h7l8 8q2 2 0 4l-6 6q-2 2-4 0l-8-8z
edit|M3 4h18v16H3z
annotate|M4 3h16q2 0 2 2v12q0 2-2 2h-9l-6 3v-3H4q-2 0-2-2V5q0-2 2-2z
addbg|M5 3h14q3 0 3 3v12q0 3-3 3H5q-3 0-3-3V6q0-3 3-3z
crop|M6 3h12q3 0 3 3v12q0 3-3 3H6q-3 0-3-3V6q0-3 3-3z
redact|M2 12Q12-2 22 12Q12 26 2 12z
trim|M4 6q0-3 3-3t3 3q0 3-3 3T4 6M4 18q0-3 3-3t3 3q0 3-3 3t-3-3
speed|M2 17a10 10 0 0 1 20 0q0 3-3 3H5q-3 0-3-3z
join|M2 7q0-3 3-3h3q3 0 3 3v10q0 3-3 3H5q-3 0-3-3zM13 7q0-3 3-3h3q3 0 3 3v10q0 3-3 3h-3q-3 0-3-3z
frames|M5 2h14q3 0 3 3v12H5zM2 8h14q2 0 2 2v10q0 2-2 2H4q-2 0-2-2V10q0-2 2-2z
split|M3 5q0-2 2-2h4v18H5q-2 0-2-2zM15 3h4q2 0 2 2v14q0 2-2 2h-4z
normalize|M3 10a1.5 1.5 0 0 1 3 0v4a1.5 1.5 0 0 1-3 0zM8 5a1.5 1.5 0 0 1 3 0v14a1.5 1.5 0 0 1-3 0zM13 8a1.5 1.5 0 0 1 3 0v8a1.5 1.5 0 0 1-3 0zM18 3a1.5 1.5 0 0 1 3 0v18a1.5 1.5 0 0 1-3 0z
visualizer|M2 12Q5 2 8 12t6 0 8 0v7H2z
channels|M3 4q0-2 2-2h4q2 0 2 2v16q0 2-2 2H5q-2 0-2-2zM13 4q0-2 2-2h4q2 0 2 2v16q0 2-2 2h-4q-2 0-2-2z
bleep|M3 8h4l6-5v18l-6-5H3q-1 0-1-1V9q0-1 1-1z
merge|M3 3h9q2 0 2 2v9H5q-2 0-2-2zM10 10h9q2 0 2 2v7q0 2-2 2h-7q-2 0-2-2z
organize|M3 5q0-2 2-2h4q2 0 2 2v4q0 2-2 2H5q-2 0-2-2zM13 5q0-2 2-2h4q2 0 2 2v4q0 2-2 2h-4q-2 0-2-2zM3 15q0-2 2-2h4q2 0 2 2v4q0 2-2 2H5q-2 0-2-2zM13 15q0-2 2-2h4q2 0 2 2v4q0 2-2 2h-4q-2 0-2-2z
share|M3 13q0-3 3-3h12q3 0 3 3v6q0 3-3 3H6q-3 0-3-3z
paperclip|M4 9a8 8 0 0 1 16 0v8a6 6 0 0 1-12 0V8a3 3 0 0 1 6 0v9H4z
float|M3 6l9-4 9 4v4l-9 4-9-4zM3 15l9 4 9-4v4l-9 4-9-4z
copy|M3 5q0-3 3-3h10v4H8q-2 0-2 2v10H3zM10 9h9q3 0 3 3v7q0 3-3 3h-9q-3 0-3-3v-7q0-3 3-3z
move|M3 10h6V4l3-3 3 3v6h6l2 2-2 2h-6v6l-3 3-3-3v-6H3l-2-2z
copypath|M2 8a5 5 0 0 1 5-5h5v7H8v4H2zM12 14h4v-4h6v6a5 5 0 0 1-5 5h-5z
terminal|M5 3h14q3 0 3 3v12q0 3-3 3H5q-3 0-3-3V6q0-3 3-3z
convert|M3 7h14l-3-3 2-2 7 7-7 7-2-2 3-3H3zM21 17H7l3 3-2 2-7-7 3-3 3 3h14z
extract|M3 5q0-2 2-2h14q2 0 2 2v3H3zM4 10h16v9q0 3-3 3H7q-3 0-3-3z
archive|M3 5q0-2 2-2h14q2 0 2 2v3H3zM4 10h16v9q0 3-3 3H7q-3 0-3-3z
qr|M2 2h8v8H2zM14 2h8v8h-8zM2 14h8v8H2z
removeaudio|M3 8h4l6-5v18l-6-5H3q-1 0-1-1V9q0-1 1-1z
rotate|M12 2a10 10 0 1 0 10 10h-4a6 6 0 1 1-6-6z
trash|M6 7h12l-1 13q0 2-2 2H9q-2 0-2-2zM4 3h16v3H4z
play|M6 5q0-3 3-1l12 7q2 1 0 2L9 20q-3 2-3-1z
pause|M5 5q0-2 2-2h1q2 0 2 2v14q0 2-2 2H7q-2 0-2-2zM14 5q0-2 2-2h1q2 0 2 2v14q0 2-2 2h-1q-2 0-2-2z
stop|M7 3h10q4 0 4 4v10q0 4-4 4H7q-4 0-4-4V7q0-4 4-4z
folder|M2 6q0-3 3-3h4l3 3h7q3 0 3 3v10q0 3-3 3H5q-3 0-3-3z
image|M5 3h14q3 0 3 3v12q0 3-3 3H5q-3 0-3-3V6q0-3 3-3z
video|M5 4h10q3 0 3 3v10q0 3-3 3H5q-3 0-3-3V7q0-3 3-3zM18 10l5-4v12l-5-4z
audio|M8 6l13-3v14a3 3 0 1 1-3-3V8l-7 2v9a3 3 0 1 1-3-3z
document|M6 2h8l7 7v11q0 2-2 2H6q-3 0-3-3V5q0-3 3-3z
warning|M10 3q2-3 4 0l9 16q1 3-2 3H3q-3 0-2-3z
mono|M12 2a10 10 0 1 0 0 20z
pin|M8 2h8q2 0 1 2l-1 5 3 4q1 2-1 2H6q-2 0-1-2l3-4-1-5q-1-2 1-2z
settings|M3 4h18v16H3z
star|M12 2l3 6 7 1-5 5 1 7-6-3-6 3 1-7-5-5 7-1z
layers|M2 7l10-5 10 5-10 5zM2 12l10 5 10-5v5l-10 5-10-5z
text|M3 3h18v5h-6v14H9V8H3z
rect|M6 4h12q4 0 4 4v8q0 4-4 4H6q-4 0-4-4V8q0-4 4-4z
circle|M12 2a10 10 0 1 0 .01 0
pen|M3 21l1-6L16 3q2-2 4 0t0 4L8 20z
highlighter|M4 20l3-5 9-12q1-2 3 0l3 3-11 15H4z
numbered|M12 2a10 10 0 1 0 .01 0
zoom|M10 2a8 8 0 1 0 .01 0M16 15l6 6-2 2-6-6z
magnifier|M10 2a8 8 0 1 0 .01 0M16 15l6 6-2 2-6-6z''')
pebble_ink=table('''compress|M5 5l5 5M10 6v4H6M19 19l-5-5M14 18v-4h4
metadata|M7 7h.01M9 12l4 4M12 9l5 5
edit|M4 7h16M4 17h16M9 5v4M15 15v4
annotate|M7 14l1-4 7-5 3 3-7 5z
addbg|M4 16l5-6 5 5 3-3 4 5M16 5v5M13.5 7.5h5
crop|M7 3v14h14M3 7h14v14
redact|M8 12a4 4 0 1 0 .01 0M4 4l16 16
trim|M8 8l13 12M8 16L21 4
speed|M5 15a7 7 0 0 1 14 0M12 15l4-6
join|M4 7v10M20 7v10M8 12h8M13 9l3 3-3 3
frames|M3 11h12v9H3zM8 3h13v12
split|M12 3v18M6 9l-3 3 3 3M18 9l3 3-3 3
normalize|M3 12h18
visualizer|M2 12q3-10 6 0t6 0 8 0
channels|M5 8v8M8 5v14M16 8v8M19 5v14
bleep|M17 7q6 5 0 10M16 10q2 2 0 4
merge|M4 4h8v8H4zM12 12h8v8h-8z
organize|M5 6h4M15 6h4M5 16h4M15 16h4
share|M12 16V2M7 7l5-5 5 5M6 17h12
paperclip|M8 16V7q0-4 4-4t4 4v10q0 5-5 5t-5-5V9
float|M3 6l9 4 9-4M3 15l9 4 9-4
copy|M11 13h7M11 17h7
move|M12 2v20M2 12h20M9 5l3-3 3 3M9 19l3 3 3-3M5 9l-3 3 3 3M19 9l3 3-3 3
copypath|M7 16L17 8
terminal|M6 8l5 4-5 4M13 16h5
convert|M3 9h16M21 17H5
extract|M12 10v9M8 15l4 4 4-4
archive|M9 13h6
qr|M4 4h4v4H4zM16 4h4v4h-4zM4 16h4v4H4zM14 14h3v3h4v4h-7z
removeaudio|M17 8l5 8M22 8l-5 8
rotate|M16 2h6v6M22 2l-7 7
trash|M9 10v8M15 10v8M9 3V2h6v1
folder|M3 10h18M7 15h7
image|M3 17l6-6 5 4 3-3 4 6M16 7h.01
video|M8 8l6 4-6 4z
prior|M12 3v18
warning|M12 8v6M12 18h.01
mono|M12 2a10 10 0 0 1 0 20M12 2v20
pin|M12 15v7
settings|M4 7h16M4 17h16M15 5v4M9 15v4
layers|M2 7l10 5 10-5M2 17l10 5 10-5
pen|M5 15l4 4M14 5l5 5
highlighter|M8 14l6 5M3 22h12
numbered|M10 9l2-1v9M10 17h4
zoom|M10 4a6 6 0 1 0 0 12a6 6 0 1 0 0-12z
magnifier|M10 4a6 6 0 1 0 0 12a6 6 0 1 0 0-12zM7 10h6M10 7v6''')
# Facet: asymmetrical folded planes and inset operation marks; no circular badge.
facet_planes=table('''document|M4 2h10l6 7v13H4z
metadata|M3 4h10l9 8-9 10L3 12z
folder|M2 5h8l3 3h9l-3 13H2z
image|M2 5h19v13l-5 4H2z
video|M2 4h16v5l5-3v13l-5-3v5H2z
audio|M5 7l15-4v15l-5 4H5z
archive|M2 4h20v5l-3 13H5L2 9z
extract|M2 4h20v5l-3 13H5L2 9z
terminal|M2 3h20v15l-5 4H2z
copy|M2 2h13v5h7v12l-4 3H7v-7H2z
float|M2 7l10-5 10 5v10l-10 5-10-5z
layers|M2 7l10-5 10 5v10l-10 5-10-5z
organize|M2 3h8v8H2zM13 3h9v8h-9zM2 14h8v8H2zM13 14h9v8h-9z
join|M2 5h8v16H2zM14 3h8v16h-8z
merge|M2 3h11v10H2zM10 11h12v11H10z
frames|M8 2h14v14H8zM2 8h14v14H2z
crop|M3 5h14l4 4v12H7l-4-4z
redact|M2 12l7-7h6l7 7-7 7H9z
speed|M2 18V9l6-6h8l6 6v9l-5 4H7z
share|M2 12h7V3h6v9h7v10H2z
move|M9 2h6v7h7v6h-7v7H9v-7H2V9h7z
convert|M2 5h14V2l6 6-6 6v-3H2zM22 16H8v-3l-6 6 6 3v-2h14z
split|M2 4h8v18H2zM14 2h8v18h-8z
normalize|M2 9h4v9H2zM7 4h4v18H7zM12 7h4v11h-4zM17 2h4v20h-4z
channels|M2 4h8v18H2zM14 2h8v18h-8z
bleep|M2 9h5l7-6v19l-7-6H2z
removeaudio|M2 9h5l7-6v19l-7-6H2z
trash|M4 7h16l-3 15H7zM2 3h20v3H2z
pin|M6 2h12l-2 8 4 5H4l4-5zM11 15h2v8h-2z
pen|M2 22l2-8L16 2l6 6L10 20z
highlighter|M3 22l4-9L16 2l6 6-9 14z
text|M2 3h20v5h-7v14H9V8H2z
rect|M2 5h20v13l-4 4H2z
circle|M7 2h10l5 5v10l-5 5H7l-5-5V7z
star|M12 2l3 6 7 1-5 5 1 8-6-4-6 4 1-8-5-5 7-1z''')
# Foreground marks use new Circuit routes; plane ink is independent from the low-opacity volume.
facet_details=table('''document|M14 2v7h6M7 13h10M7 17h7
metadata|M6 7h.01M9 12l5 5M12 9l5 5
folder|M2 10h20M5 16h10
image|M3 18l6-7 5 5 3-3 4 4M16 8h2
video|M7 8l7 4-7 4zM18 9v7
audio|M8 16V8l9-2v10M8 16h-3M17 16h-3
archive|M2 9h20M10 13h4v3h-4z
extract|M2 9h20M12 11v8M8 15l4 4 4-4
terminal|M2 7h20M6 10l4 4-4 4M12 18h5M17 22v-4h5
copy|M7 15V7h8M11 12h7M11 16h7M18 22v-3h4
float|M2 7l10 5 10-5M12 12v10M2 15l10 5 10-5
layers|M2 7l10 5 10-5M12 12v10M2 15l10 5 10-5
organize|M5 5h4M16 5h5M5 16h4M16 16h5
join|M5 8v10M19 6v10M8 12h8
merge|M2 3h11v10H2zM10 11h12v11H10z
frames|M8 2h14v14M2 8h14v14H2z
crop|M7 2v15h15M2 7h15v15
redact|M7 8l10 8M5 4l15 16
speed|M5 16l3-8h8l3 8M12 16l4-7
share|M12 15V3M8 7l4-4 4 4M5 17h14
move|M12 2v20M2 12h20M9 5l3-3 3 3M9 19l3 3 3-3M5 9l-3 3 3 3M19 9l3 3-3 3
convert|M3 8h16M6 5l-3 3 3 3M21 19H5M18 16l3 3-3 3
split|M6 8l-3 4 3 4M18 6l3 4-3 4
normalize|M2 12h19
channels|M5 8v10M8 6v14M17 6v10M20 4v14
bleep|M17 6l5 6-5 6
removeaudio|M17 8l5 8M22 8l-5 8
trash|M8 10l2 8M16 10l-2 8
pin|M8 5h8M9 12h6
pen|M4 14l6 6M14 4l6 6
highlighter|M7 13l6 9M3 22h10
text|M3 3h18M12 8v14
rect|M18 22v-4h4M2 5h20
circle|M7 2l10 20M17 2L7 22
star|M12 2v11l-6 9M12 13l10-4''')
# Directional planes create an asymmetric folded body, rather than a second outline family.
facet_shades=table('''document|M14 2v7h6zM4 22l16-13v13z
metadata|M13 4l9 8-9 10z
folder|M2 10h20l-3 11H2z
image|M16 22v-4h5zM2 5l19 13H2z
video|M18 9l5-3v13l-5-3zM2 4l16 17H2z
audio|M5 7l15-4v6L5 13z
archive|M2 4l10 5 10-5v5H2zM12 9h10l-3 13h-7z
extract|M2 4l10 5 10-5v5H2zM12 9h10l-3 13h-7z
terminal|M17 22v-4h5zM2 3l20 15H2z
copy|M2 2l13 5H7v8H2zM18 22v-3h4z
float|M12 12l10-5v10l-10 5z
layers|M12 12l10-5v10l-10 5z
organize|M2 3l8 8H2zM13 3l9 8h-9zM2 14l8 8H2zM13 14l9 8h-9z
join|M2 5l8 16H2zM14 3l8 16h-8z
merge|M2 3l11 10H2zM10 11l12 11H10z
frames|M8 2l14 14H8zM2 8l14 14H2z
crop|M3 5l4 12h14v4H7l-4-4z
redact|M2 12h20l-7 7H9z
speed|M2 18l10-6 10 6-5 4H7z
share|M2 12l7 5h13v5H2z
move|M12 2v20H9v-7H2v-3h10z
convert|M2 5l20 3-6 6v-3H2zM2 19l20-3v4H8v2z
split|M2 4l8 18H2zM14 2l8 18h-8z
normalize|M2 9l4 9H2zM7 4l4 18H7zM12 7l4 11h-4zM17 2l4 20h-4z
channels|M2 4l8 18H2zM14 2l8 18h-8z
bleep|M7 9l7-6v19l-7-6z
removeaudio|M7 9l7-6v19l-7-6z
trash|M4 7l8 15h5l3-15z
pin|M12 2h6l-2 8 4 5h-8z
pen|M2 22l20-14-12 12z
highlighter|M3 22L22 8l-9 14z
text|M12 3h10v5h-7v14h-3z
rect|M18 22v-4h4zM2 5l16 13H2z
circle|M12 2h5l5 5v10l-5 5h-5z
star|M12 2v11l10-4-5 5 1 8-6-4z''')
# Utility controls use purpose-built compact filled silhouettes (no extra surrounding container).
solid_controls={'play','pause','stop','chevronup','chevrondown','chevronleft','chevronright','add','remove','grip','more'}
for pack_id, title, description, round_caps in [
 ('pebble','Pebble','Soft silhouettes. Rounded forms with quiet two-tone depth.',True),
 ('circuit','Circuit','Signal and structure. Square terminals, precise routes and open geometry.',False),
 ('facet','Facet','Folded planes. Angular volumes and crisp operation marks.',False)]:
 icons={}
 for n,(old,filled) in classic.items():
  c=circuit[n]
  if pack_id=='circuit':
   icons[n]=[line(c,1.55)] if n not in solid_controls else [fill(c)]
  elif pack_id=='pebble':
   if n in body:
    # Detailed solids have soft volume; core operation remains full foreground ink.
    ink=pebble_ink.get(n)
    icons[n]=[fill(body[n],.23),line(ink,1.95)] if ink else [fill(body[n])]
   elif n in solid_controls:
    # Utility marks intentionally compact and generous at 16 DIP.
    icons[n]=[fill(c)]
   else:
    icons[n]=[line(c,2.5)]
  else:
   if n in facet_planes:
    plane=facet_planes[n];detail=facet_details[n]
    icons[n]=[fill(plane,.18),fill(facet_shades[n],.42),line(detail,1.65)]
    # One directional fold, not a universal badge or a fake cast shadow.
   elif n=='compress':
    icons[n]=[fill('M3 3l9 4-5 5zM21 21l-9-4 5-5z'),fill('M3 3l4 9 5-5zM21 21l-4-9-5 5z',.4)]
   elif n in solid_controls:
    icons[n]=[fill(c)]
   elif n in ('check','close','arrow','line','swap','rotate','reset','visualizer','paperclip','copypath'):
    icons[n]=[line(c,2.4)]
   else:
    # Octagonal magnifying lenses, warning markers and count stamps.
    icons[n]=[fill(c,.22),line(c,1.9)]
 manifest=dict(schema=1,id=pack_id,name=title,description=description,round=round_caps,icons=icons)
 dest=ROOT/'assets/icon-packs'/f'{["pebble","circuit","facet"].index(pack_id)+1:02}-{pack_id}.json'
 dest.parent.mkdir(parents=True,exist_ok=True)
 dest.write_text(json.dumps(manifest,indent=2)+'\n',encoding='utf-8')
 print(f'{dest.name}: {len(icons)} icons')
assert set(classic)==set(circuit),set(classic)^set(circuit)
