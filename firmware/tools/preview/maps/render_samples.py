# /// script
# dependencies = ["pillow"]
# ///
"""Renders the preview's sample maps from OpenStreetMap's standard tiles, turned dark, the way the
phone hands a map to the display: a 1024 x 1024 baseline JPEG, north up, its centre pixel at
lat/lon, `mpp` metres per pixel in the protocol's flat projection (see PROTOCOL.md). Next to each
goes a .json with that metadata, a drive route along the real streets (map pixels, routed through
OpenStreetMap's road network between the waypoints below) and the car's place and heading on it.

    uv run tools/preview/maps/render_samples.py

Tiles come from tile.openstreetmap.org and roads from the Overpass API (© OpenStreetMap
contributors, ODbL; a few dozen requests, within their usage policies); both are cached in
~/.cache/monav-tiles, so a rerun asks for nothing. Monav
itself draws its maps with Apple's MapKit; any picture with the same metadata works.
"""
import hashlib
import heapq
import json
import math
import time
import urllib.parse
import urllib.request
from io import BytesIO
from pathlib import Path

from PIL import Image, ImageEnhance, ImageOps

HERE = Path(__file__).parent
CACHE = Path.home() / ".cache" / "monav-tiles"
TILE = 256
URL = "https://tile.openstreetmap.org/{z}/{x}/{y}.png"
ATTR = "© OpenStreetMap"
SIZE = 1024

ROADS = "motorway|trunk|primary|secondary|tertiary|unclassified|residential|living_street|motorway_link|trunk_link|primary_link|secondary_link|tertiary_link"

SAMPLES = {
    # name: centre, metres per pixel, the speed the preview drives there, the road types to route
    # over, waypoints (map pixels, snapped to the nearest road node; the route runs between them
    # on the shortest way) and how far along the route the car is (metres)
    "sample-berliner-allee": (51.2211, 6.7841, 1.6009712156669653, 50, ROADS,
                              # Corneliusstraße / Berliner Allee north, right into Bahnstraße, left into Oststraße
                              [(452, 975), (428, 606), (520, 607), (572, 470)], 160),
    "sample-koenigsallee": (51.2236, 6.7792, 1.0006065807011368, 30, ROADS,
                            # Königsallee north, right into Steinstraße
                            [(518, 905), (518, 574), (820, 574)], 140),
    "sample-a46": (51.188, 6.825, 3.2019606307976796, 120, "motorway",
                   # the A 46 eastwards
                   [(330, 358), (955, 445)], 340),
}


def tile(z, x, y):
    path = CACHE / f"osm/{z}/{x}/{y}.png"
    if not path.exists():
        path.parent.mkdir(parents=True, exist_ok=True)
        req = urllib.request.Request(URL.format(z=z, x=x, y=y),
                                     headers={"User-Agent": "monav-display-preview/1.0 (sample maps for a README)"})
        path.write_bytes(urllib.request.urlopen(req, timeout=30).read())
        time.sleep(0.1)
    return Image.open(BytesIO(path.read_bytes())).convert("RGB")


def roads(lat0, lon0, mpp, types):
    """Drivable ways around the image: {node: (x, y)} in map pixels, and their edges."""
    half = SIZE / 2 * mpp * 1.2
    dlat, dlon = half / 110540, half / (111320 * math.cos(math.radians(lat0)))
    query = (f'[out:json][timeout:90];way["highway"~"^({types})$"]'
             f'({lat0 - dlat},{lon0 - dlon},{lat0 + dlat},{lon0 + dlon});out geom;')
    path = CACHE / "overpass" / f"{hashlib.sha1(query.encode()).hexdigest()[:16]}.json"
    if not path.exists():
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(overpass(query))
    nodes, edges = {}, {}
    for way in json.loads(path.read_text())["elements"]:
        ids = way["nodes"]
        for nid, g in zip(ids, way["geometry"]):
            nodes[nid] = pixel(lat0, lon0, mpp, g["lat"], g["lon"])
        for a, b in zip(ids, ids[1:]):
            d = math.dist(nodes[a], nodes[b])
            edges.setdefault(a, []).append((b, d))
            edges.setdefault(b, []).append((a, d))
    return nodes, edges


def overpass(query):
    """The public Overpass servers are often busy: a few tries, two servers."""
    servers = ["https://overpass-api.de/api/interpreter", "https://overpass.private.coffee/api/interpreter"]
    for attempt in range(6):
        req = urllib.request.Request(servers[attempt % 2], data=urllib.parse.urlencode({"data": query}).encode(),
                                     headers={"User-Agent": "monav-display-preview/1.0 (sample maps for a README)"})
        try:
            return urllib.request.urlopen(req, timeout=180).read()
        except OSError as err:
            print(f"  overpass: {err}, trying again")
            time.sleep(5 * (attempt + 1))
    raise SystemExit("Overpass did not answer")


def pixel(lat0, lon0, mpp, lat, lon):
    """The protocol's flat projection, as map pixels."""
    east = (lon - lon0) * 111320 * math.cos(math.radians(lat0))
    north = (lat - lat0) * 110540
    return SIZE / 2 + east / mpp, SIZE / 2 - north / mpp


def shortest(edges, a, b):
    dist, prev, queue = {a: 0.0}, {}, [(0.0, a)]
    while queue:
        d, n = heapq.heappop(queue)
        if n == b:
            break
        if d > dist[n]:
            continue
        for m, w in edges.get(n, ()):
            if d + w < dist.get(m, math.inf):
                dist[m], prev[m] = d + w, n
                heapq.heappush(queue, (d + w, m))
    out = [b]
    while out[-1] != a:
        out.append(prev[out[-1]])
    return out[::-1]


def simplify(pts, tol=0.8):
    """Douglas-Peucker in map pixels."""
    if len(pts) < 3:
        return pts
    (ax, ay), (bx, by) = pts[0], pts[-1]
    length = math.hypot(bx - ax, by - ay) or 1e-9
    i, far = max(((i, abs((bx - ax) * (ay - y) - (ax - x) * (by - ay)) / length) for i, (x, y) in enumerate(pts[1:-1], 1)),
                 key=lambda t: t[1])
    if far <= tol:
        return [pts[0], pts[-1]]
    return simplify(pts[:i + 1], tol)[:-1] + simplify(pts[i:], tol)


def route(lat0, lon0, mpp, types, waypoints, car_m):
    nodes, edges = roads(lat0, lon0, mpp, types)
    # Every waypoint on the part of the network the first one reaches (a motorway's carriageways
    # are separate ways that only meet beyond the image).
    first = min(nodes, key=lambda n: math.dist(nodes[n], waypoints[0]))
    reach, todo = {first}, [first]
    while todo:
        for m, _ in edges.get(todo.pop(), ()):
            if m not in reach:
                reach.add(m)
                todo.append(m)
    snap = [first] + [min(reach, key=lambda n: math.dist(nodes[n], w)) for w in waypoints[1:]]
    ids = [snap[0]]
    for a, b in zip(snap, snap[1:]):
        ids += shortest(edges, a, b)[1:]
    pts = simplify([nodes[n] for n in ids])
    # The car `car_m` along it, heading along the segment it is on (degrees from north).
    left = car_m / mpp
    for (ax, ay), (bx, by) in zip(pts, pts[1:]):
        seg = math.hypot(bx - ax, by - ay)
        if left <= seg:
            t = left / seg
            hdg = math.degrees(math.atan2(bx - ax, ay - by)) % 360
            car = [round(ax + (bx - ax) * t, 1), round(ay + (by - ay) * t, 1), round(hdg, 1)]
            break
        left -= seg
    return [[round(x, 1), round(y, 1)] for x, y in pts], car


def world(lat, lon, z):
    """Web Mercator pixel at zoom z."""
    n = 2 ** z * TILE
    s = math.sin(math.radians(lat))
    return (lon + 180) / 360 * n, (0.5 - math.log((1 + s) / (1 - s)) / (4 * math.pi)) * n


def dark(img):
    """The bright standard style as a night map: inverted, hues turned back, dimmed."""
    h, s, v = ImageOps.invert(img).convert("HSV").split()
    h = h.point(lambda x: (x + 128) % 256)
    img = Image.merge("HSV", (h, s, v)).convert("RGB")
    img = ImageEnhance.Color(img).enhance(0.55)
    img = ImageEnhance.Contrast(img).enhance(1.15)
    return ImageEnhance.Brightness(img).enhance(0.8)


def render(lat0, lon0, mpp):
    # The protocol's flat projection around the centre (PROTOCOL.md, "Map image").
    def geo(px, py):
        east, north = (px - SIZE / 2) * mpp, (SIZE / 2 - py) * mpp
        return lat0 + north / 110540, lon0 + east / (111320 * math.cos(math.radians(lat0)))

    # The first zoom whose pixels are at least as fine as the output's.
    z = math.ceil(math.log2(156543.03 * math.cos(math.radians(lat0)) / mpp))
    corners = [world(*geo(px, py), z) for px, py in ((0, 0), (SIZE, 0), (0, SIZE), (SIZE, SIZE))]
    x0, y0 = int(min(c[0] for c in corners) // TILE), int(min(c[1] for c in corners) // TILE)
    x1, y1 = int(max(c[0] for c in corners) // TILE), int(max(c[1] for c in corners) // TILE)
    mosaic = Image.new("RGB", ((x1 - x0 + 1) * TILE, (y1 - y0 + 1) * TILE))
    for ty in range(y0, y1 + 1):
        for tx in range(x0, x1 + 1):
            mosaic.paste(tile(z, tx, ty), ((tx - x0) * TILE, (ty - y0) * TILE))
    # Over a few kilometres the two projections differ by far less than a pixel: one affine map.
    (ax, ay), (bx, by), (cx, cy), _ = [(c[0] - x0 * TILE, c[1] - y0 * TILE) for c in corners]
    coeffs = ((bx - ax) / SIZE, (cx - ax) / SIZE, ax, (by - ay) / SIZE, (cy - ay) / SIZE, ay)
    return dark(mosaic.transform((SIZE, SIZE), Image.Transform.AFFINE, coeffs, Image.Resampling.BICUBIC)), z


for name, (lat, lon, mpp, kmh, types, waypoints, car_m) in SAMPLES.items():
    img, z = render(lat, lon, mpp)
    img.save(HERE / f"{name}.jpg", quality=86, optimize=True)  # baseline, as the display decodes
    pts, car = route(lat, lon, mpp, types, waypoints, car_m)
    meta = {"w": SIZE, "h": SIZE, "lat": lat, "lon": lon, "mpp": mpp, "kmh": kmh, "attr": ATTR}
    lines = [f"  {json.dumps(k)}: {json.dumps(v, ensure_ascii=False)}," for k, v in meta.items()]
    lines.append(f'  "car": {json.dumps(car)},')
    lines.append('  "route": [\n' + ",\n".join(f"    {json.dumps(p)}" for p in pts) + "\n  ]")
    (HERE / f"{name}.json").write_text("{\n" + "\n".join(lines) + "\n}\n")
    print(f"{name}: zoom {z}, {(HERE / f'{name}.jpg').stat().st_size // 1024} kB, route {len(pts)} points, car {car}")
