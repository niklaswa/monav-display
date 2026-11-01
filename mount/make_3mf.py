"""Packs the exported STLs into Bambu Studio-ready 3MF plates (one object per part)."""
import struct, sys, zipfile
from pathlib import Path

HERE = Path(__file__).parent
PLATES = {
    "monav-mount.3mf": [("ring", 98, 128), ("lid", 158, 128), ("nut", 128, 178)],
    "monav-test.3mf": [("test_socket", 108, 128), ("nut", 150, 128)],
    # lid and knob only, for reprinting the socket without the ring
    "monav-socket.3mf": [("lid", 105, 128), ("nut", 165, 128)],
}


def read_stl(path):
    data = path.read_bytes()
    n = struct.unpack("<I", data[80:84])[0]
    verts, index, tris = [], {}, []
    for i in range(n):
        v = struct.unpack("<12f", data[84 + i * 50: 84 + i * 50 + 48])
        tri = []
        for k in range(3):
            p = tuple(round(c, 5) for c in v[3 + 3 * k: 6 + 3 * k])
            if p not in index:
                index[p] = len(verts)
                verts.append(p)
            tri.append(index[p])
        if len(set(tri)) == 3:
            tris.append(tri)
    return verts, tris


def model_xml(items):
    out = ['<?xml version="1.0" encoding="UTF-8"?>',
           '<model unit="millimeter" xml:lang="en-US" xmlns="http://schemas.microsoft.com/3dmanufacturing/core/2015/02">',
           "<resources>"]
    build = []
    for oid, (name, x, y) in enumerate(items, start=1):
        verts, tris = read_stl(HERE / "stl" / f"monav-{name}.stl")
        out.append(f'<object id="{oid}" name="monav-{name}" type="model"><mesh><vertices>')
        out.extend(f'<vertex x="{a}" y="{b}" z="{c}"/>' for a, b, c in verts)
        out.append("</vertices><triangles>")
        out.extend(f'<triangle v1="{a}" v2="{b}" v3="{c}"/>' for a, b, c in tris)
        out.append("</triangles></mesh></object>")
        build.append(f'<item objectid="{oid}" transform="1 0 0 0 1 0 0 0 1 {x} {y} 0"/>')
    out += ["</resources>", "<build>", *build, "</build>", "</model>"]
    return "\n".join(out)


CONTENT_TYPES = """<?xml version="1.0" encoding="UTF-8"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>
<Default Extension="model" ContentType="application/vnd.ms-package.3dmanufacturing-3dmodel+xml"/>
</Types>"""
RELS = """<?xml version="1.0" encoding="UTF-8"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
<Relationship Target="/3D/3dmodel.model" Id="rel0" Type="http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel"/>
</Relationships>"""

for name, items in PLATES.items():
    with zipfile.ZipFile(HERE / name, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("[Content_Types].xml", CONTENT_TYPES)
        z.writestr("_rels/.rels", RELS)
        z.writestr("3D/3dmodel.model", model_xml(items))
    print(name, (HERE / name).stat().st_size // 1024, "KB")
