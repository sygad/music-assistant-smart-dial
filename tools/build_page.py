#!/usr/bin/env python3
"""Build index.html (the GitHub Pages version of the story) from STORY.md.

STORY.md stays the one source. GitHub renders it to HTML (`gh api markdown`), this script
reshapes that HTML for the page (figures with captions, numbered headings, the timeline) and
drops it into tools/page.html, which adds the pop-up image viewer.

Run from the repo root:  python3 tools/build_page.py   (needs the GitHub CLI, signed in)
"""
import json
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
REPO = "sygad/music-assistant-smart-dial"
FULL_SIZE = f"https://github.com/{REPO}/raw/main/images/"

readme = (ROOT / "STORY.md").read_text()
html = subprocess.run(
    ["gh", "api", "markdown", "--input", "-"],
    input=json.dumps({"text": readme, "mode": "gfm"}),
    capture_output=True, text=True, check=True,
).stdout

# The README's blank-line spacers: the page spaces its own sections.
html = re.sub(r"^<br>\n", "", html, flags=re.M)

# On the page, an image opens in the pop-up, from the file next to the page.
html = html.replace(FULL_SIZE, "images/")
html = html.replace("<strong>Click any image to open it full size.</strong>",
                    "<strong>Click any image to see it larger.</strong>")

def image_link(m):
    href, img = m[1], re.sub(r' style="[^"]*"', "", m[2])
    if "-small.png" in img:  # cut-out product photo: no shadow
        img = img.replace("<img ", '<img class="transparent" ', 1)
    return f'<a class="zoom" href="{href}">{img}</a>'

html = re.sub(r'<a href="(images/[^"]+)"[^>]*>(<img [^>]*>)</a>', image_link, html)

# An image paragraph and the italic caption under it become one figure.
html = re.sub(
    r"<p>(<a class=\"zoom\"[^>]*><img [^>]*></a>)</p>\n<p><em>(.*?)</em></p>",
    r"<figure>\1<figcaption>\2</figcaption></figure>", html, flags=re.S)
html = re.sub(r"<p>(<a class=\"zoom\"[^>]*><img [^>]*></a>)</p>", r"<figure>\1</figure>", html)

# "## 2. The ceiling speakers (May to October 2024)": number, title and dates styled apart.
def heading(m):
    num, title = m[1], m[2]
    when = re.search(r"\s*\(([^)]*)\)$", title)
    if when:
        title = title[:when.start()]
        return f'<h2><span class="num">{num}</span><span>{title}</span><span class="when">{when[1]}</span></h2>'
    return f'<h2><span class="num">{num}</span><span>{title}</span></h2>'

html = re.sub(r"<h2>(\d+)\. (.*?)</h2>", heading, html)

# The story at a glance: dates in their own column.
def timeline(m):
    items = re.sub(r"<li><strong>(.*?)</strong> · (.*?)</li>",
                   r'<li><span class="date">\1</span><span>\2</span></li>', m[2], flags=re.S)
    return f'{m[1]}<ul class="timeline">{items}</ul>'

html = re.sub(r"(<h2>The story at a glance</h2>\n)<ul>(.*?)</ul>", timeline, html, flags=re.S)

# Links to other sites open in a new tab, so the story keeps its place.
html = re.sub(r'<a href="(https?://[^"]+)"( rel="nofollow")?',
              r'<a href="\1" target="_blank" rel="noopener"', html)
html = html.replace('<a href="LICENSE"', f'<a href="https://github.com/{REPO}/blob/main/LICENSE" target="_blank" rel="noopener"')

page = (ROOT / "tools" / "page.html").read_text().replace("<!-- CONTENT -->", html.strip())
(ROOT / "index.html").write_text(page)
print("index.html:", page.count('class="zoom"'), "images,", page.count("<figcaption>"), "captions")
