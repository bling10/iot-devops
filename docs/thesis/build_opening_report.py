"""Build the opening-report DOCX with Python's standard library only.

The document skill's preferred python-docx runtime is unavailable in this
workspace, so this script writes the small set of required OOXML parts directly.
"""

from pathlib import Path
from html import escape
from zipfile import ZipFile, ZIP_DEFLATED
from xml.etree import ElementTree
import re


ROOT = Path(__file__).resolve().parent
SOURCE = ROOT / "开题报告_草稿.md"
OUTPUT = ROOT / "开题报告_草稿.docx"
NS = 'xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"'


def x(text):
    return escape(text, quote=True)


def run(text):
    return '<w:r><w:t xml:space="preserve">' + x(text) + '</w:t></w:r>'


def paragraph(text, style):
    return '<w:p><w:pPr><w:pStyle w:val="' + style + '"/></w:pPr>' + run(text) + '</w:p>'


lines = SOURCE.read_text(encoding="utf-8").splitlines()
blocks = []
for line in lines:
    line = line.strip()
    if not line or line.startswith(">"):
        continue
    if line.startswith("# "):
        blocks.append(paragraph(line[2:].removesuffix("（草稿）"), "ReportTitle"))
    elif line.startswith("## "):
        blocks.append(paragraph(line[3:], "ReportHeading"))
    elif line.startswith("[") and re.match(r"^\[\d+\]", line):
        blocks.append(paragraph(line, "Reference"))
    else:
        # Markdown inline code/emphasis is not used in the printed draft.
        line = line.replace("`", "").replace("**", "")
        blocks.append(paragraph(line, "Normal"))

body = "".join(blocks)
document = f'''<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<w:document {NS}><w:body>{body}
<w:sectPr><w:pgSz w:w="11906" w:h="16838"/>
<w:pgMar w:top="1417" w:right="1417" w:bottom="1417" w:left="1701" w:header="709" w:footer="709" w:gutter="0"/>
<w:cols w:space="425"/><w:docGrid w:linePitch="400"/></w:sectPr>
</w:body></w:document>'''

# narrative_proposal is the selected preset. Academic-format overrides:
# A4; left 3 cm, other margins 2.5 cm; Chinese Songti/English Times New Roman;
# 12 pt body; exactly 20 pt line spacing; standard character spacing; black text.
font = '<w:rFonts w:ascii="Times New Roman" w:hAnsi="Times New Roman" w:eastAsia="宋体" w:cs="Times New Roman"/>'
base_rpr = font + '<w:sz w:val="24"/><w:szCs w:val="24"/><w:spacing w:val="0"/><w:color w:val="000000"/><w:lang w:val="en-US" w:eastAsia="zh-CN"/>'
styles = f'''<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<w:styles {NS}>
<w:docDefaults><w:rPrDefault><w:rPr>{base_rpr}</w:rPr></w:rPrDefault>
<w:pPrDefault><w:pPr><w:spacing w:before="0" w:after="0" w:line="400" w:lineRule="exact"/></w:pPr></w:pPrDefault></w:docDefaults>
<w:style w:type="paragraph" w:default="1" w:styleId="Normal"><w:name w:val="Normal"/>
<w:pPr><w:jc w:val="both"/><w:ind w:firstLine="480"/>
<w:spacing w:before="0" w:after="0" w:line="400" w:lineRule="exact"/></w:pPr>
<w:rPr>{base_rpr}</w:rPr></w:style>
<w:style w:type="paragraph" w:styleId="ReportTitle"><w:name w:val="Report Title"/>
<w:basedOn w:val="Normal"/><w:next w:val="Normal"/>
<w:pPr><w:jc w:val="center"/><w:ind w:firstLine="0"/>
<w:spacing w:before="0" w:after="240" w:line="400" w:lineRule="exact"/></w:pPr>
<w:rPr>{font}<w:b/><w:sz w:val="32"/><w:szCs w:val="32"/><w:spacing w:val="0"/><w:color w:val="000000"/></w:rPr></w:style>
<w:style w:type="paragraph" w:styleId="ReportHeading"><w:name w:val="Report Heading"/>
<w:basedOn w:val="Normal"/><w:next w:val="Normal"/>
<w:pPr><w:keepNext/><w:jc w:val="left"/><w:ind w:firstLine="0"/>
<w:spacing w:before="200" w:after="80" w:line="400" w:lineRule="exact"/></w:pPr>
<w:rPr>{base_rpr}<w:b/></w:rPr></w:style>
<w:style w:type="paragraph" w:styleId="Reference"><w:name w:val="Reference"/>
<w:basedOn w:val="Normal"/><w:next w:val="Reference"/>
<w:pPr><w:jc w:val="left"/><w:ind w:left="480" w:hanging="480"/>
<w:spacing w:before="0" w:after="0" w:line="400" w:lineRule="exact"/></w:pPr>
<w:rPr>{base_rpr}</w:rPr></w:style>
</w:styles>'''

content_types = '''<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>
<Default Extension="xml" ContentType="application/xml"/>
<Override PartName="/word/document.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml"/>
<Override PartName="/word/styles.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.styles+xml"/>
<Override PartName="/docProps/core.xml" ContentType="application/vnd.openxmlformats-package.core-properties+xml"/>
</Types>'''
rels = '''<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="word/document.xml"/>
<Relationship Id="rId2" Type="http://schemas.openxmlformats.org/package/2006/relationships/metadata/core-properties" Target="docProps/core.xml"/>
</Relationships>'''
doc_rels = '''<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles" Target="styles.xml"/>
</Relationships>'''
core = '''<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<cp:coreProperties xmlns:cp="http://schemas.openxmlformats.org/package/2006/metadata/core-properties" xmlns:dc="http://purl.org/dc/elements/1.1/" xmlns:dcterms="http://purl.org/dc/terms/" xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance">
<dc:title>面向 ESP32 边缘设备的 OTA 持续交付与可观测性平台设计与实现——开题报告</dc:title>
<dc:subject>毕业设计开题报告</dc:subject>
<dc:language>zh-CN</dc:language>
<dcterms:created xsi:type="dcterms:W3CDTF">2026-10-19T00:00:00Z</dcterms:created>
</cp:coreProperties>'''

with ZipFile(OUTPUT, "w", compression=ZIP_DEFLATED) as z:
    z.writestr("[Content_Types].xml", content_types)
    z.writestr("_rels/.rels", rels)
    z.writestr("word/document.xml", document)
    z.writestr("word/_rels/document.xml.rels", doc_rels)
    z.writestr("word/styles.xml", styles)
    z.writestr("docProps/core.xml", core)

with ZipFile(OUTPUT) as z:
    for part in ("word/document.xml", "word/styles.xml", "docProps/core.xml"):
        ElementTree.fromstring(z.read(part))
    saved_doc = z.read("word/document.xml").decode("utf-8")
    saved_styles = z.read("word/styles.xml").decode("utf-8")
    assert 'w:w="11906" w:h="16838"' in saved_doc
    assert 'w:left="1701"' in saved_doc and 'w:right="1417"' in saved_doc
    assert 'w:top="1417"' in saved_doc and 'w:bottom="1417"' in saved_doc
    assert 'w:line="400" w:lineRule="exact"' in saved_styles
    assert 'w:eastAsia="宋体"' in saved_styles and 'Times New Roman' in saved_styles
    assert 'w:spacing w:val="0"' in saved_styles

main = SOURCE.read_text(encoding="utf-8").split("## 五、")[0]
main = main.split("## 一、", 1)[1]
han_count = len(re.findall(r"[\u4e00-\u9fff]", main))
assert han_count >= 3000, f"body has only {han_count} Chinese characters"
print(f"Created {OUTPUT}; Chinese body characters (sections 1–4): {han_count}; paragraphs: {len(blocks)}")
